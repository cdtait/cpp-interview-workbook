// J3 — SeqLock: letting readers race, then detecting that they lost.
//
// A seqlock inverts the usual trade. Readers take no lock and never block the
// writer; instead they read optimistically and CHECK afterwards whether the data
// moved underneath them. If it did, they retry. The writer is never delayed by a
// reader, which is why this is the standard structure for a single-writer,
// many-reader hot path such as a market-data book.
//
// The protocol is a counter:
//
//   writer:   seq -> odd      (publishes "update in progress")
//             write the data
//             seq -> even     (publishes "data is consistent")
//
//   reader:   s1 = seq;  if odd, retry
//             read the data
//             s2 = seq;  if s1 != s2, the data is torn: retry
//
// Three things make this easy to get subtly wrong, and all three are invisible
// on x86:
//
//   1. THE FENCES ARE NOT WHERE THE ORDERINGS GO. After making seq odd, you
//      need the data writes not to be hoisted ABOVE it — but a release STORE
//      orders what came before it, not what comes after. That needs a release
//      FENCE. Symmetrically, the reader needs an acquire FENCE after reading the
//      data, so the reads cannot sink past the second seq load.
//   2. THE DATA MUST BE TRIVIALLY COPYABLE AND SELF-CONTAINED. A seqlock reader
//      deliberately reads bytes that may be changing. If those bytes include a
//      pointer or a size — a std::vector, a std::string — a torn read is not
//      garbage data, it is a wild pointer.
//   3. THE RACE IS FORMALLY UB unless the data is atomic. Reading a plain object
//      while another thread writes it is a data race no matter how carefully you
//      validate afterwards. The fix is per-word relaxed atomics, which cost
//      nothing on x86 and make the program well-defined.

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <thread>

#include <unistd.h>
#include <x86intrin.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kWords = 8;          // 64 bytes: one cache line of payload

// Every word holds the same value, so ANY difference between words is a torn
// read that the protocol was supposed to catch.
struct Payload {
    std::array<std::uint64_t, kWords> w{};
    [[nodiscard]] bool consistent() const noexcept {
        for (std::size_t i = 1; i < kWords; ++i) if (w[i] != w[0]) return false;
        return true;
    }
};

// ===========================================================================
// The correct SeqLock.
// ===========================================================================
class SeqLock {
public:
    void write(std::uint64_t value) noexcept {
        const auto s = seq_.load(std::memory_order_relaxed);   // single writer owns it
        seq_.store(s + 1, std::memory_order_relaxed);          // -> odd
        std::atomic_thread_fence(std::memory_order_release);   // data must NOT hoist above

        for (std::size_t i = 0; i < kWords; ++i)
            data_[i].store(value, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);   // data before the publish
        seq_.store(s + 2, std::memory_order_relaxed);          // -> even
    }

    [[nodiscard]] bool try_read(Payload& out) const noexcept {
        const auto s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1) return false;                              // writer mid-update

        for (std::size_t i = 0; i < kWords; ++i)
            out.w[i] = data_[i].load(std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_acquire);   // reads must NOT sink below
        const auto s2 = seq_.load(std::memory_order_relaxed);
        return s1 == s2;
    }

    // Spin until a consistent snapshot is obtained. Note this loop does NOT
    // consume a retry budget when it finds the writer busy — see the audit in
    // section 6 for why that distinction matters.
    // retries is incremented once per failed attempt, so the caller can see how
    // hard the reader had to work.
    void read(Payload& out, std::atomic<std::uint64_t>* retries = nullptr) const noexcept {
        while (!try_read(out)) {
            if (retries) retries->fetch_add(1, std::memory_order_relaxed);
            _mm_pause();
        }
    }

private:
    alignas(64) std::atomic<std::uint64_t> seq_{0};
    alignas(64) std::array<std::atomic<std::uint64_t>, kWords> data_{};
};

// ===========================================================================
// The mutex alternative, for cost comparison.
// ===========================================================================
class MutexProtected {
public:
    void write(std::uint64_t value) noexcept {
        std::lock_guard<std::mutex> lk(m_);
        for (std::size_t i = 0; i < kWords; ++i) data_.w[i] = value;
    }
    void read(Payload& out) const noexcept {
        std::lock_guard<std::mutex> lk(m_);
        out = data_;
    }
private:
    mutable std::mutex m_;
    Payload data_{};
};

// ---------------------------------------------------------------------------
// Disassembly helper, as in J1 and J2.
// ---------------------------------------------------------------------------
void show(const char* symbol, const char* note) {
    char exe[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = '\0';
    char cmd[PATH_MAX + 256];
    std::snprintf(cmd, sizeof(cmd),
                  "objdump -d --disassemble=%s --no-show-raw-insn '%s' 2>/dev/null", symbol, exe);
    std::FILE* pipe = popen(cmd, "r");
    if (!pipe) return;
    std::printf("   %-26s %s\n", symbol, note);
    char line[512];
    bool body = false;
    while (std::fgets(line, sizeof(line), pipe)) {
        if (std::strstr(line, ">:")) { body = true; continue; }
        if (!body) continue;
        if (line[0] == '\n') break;
        char* tab = std::strchr(line, '\t');
        const char* insn = tab ? tab + 1 : line;
        if (std::strstr(insn, "nop")) continue;
        char* hash = std::strchr(const_cast<char*>(insn), '#');
        if (hash) *hash = '\0';
        std::printf("        %s", insn);
        if (insn[std::strlen(insn) - 1] != '\n') std::putchar('\n');
    }
    pclose(pipe);
}

SeqLock g_lock;
std::atomic<std::uint64_t> g_retries{0};

}  // namespace

extern "C" {
[[gnu::noinline]] void seqlock_write(std::uint64_t v) { g_lock.write(v); }
[[gnu::noinline]] void fence_release_only() { std::atomic_thread_fence(std::memory_order_release); }
[[gnu::noinline]] void fence_acquire_only() { std::atomic_thread_fence(std::memory_order_acquire); }
[[gnu::noinline]] void fence_seq_cst_only() { std::atomic_thread_fence(std::memory_order_seq_cst); }
}

namespace {

struct ReadStats { std::uint64_t torn; std::uint64_t retries; double ns_per_read; };

// writer_pause is spin iterations between updates. 0 means a writer that never
// lets go, which is the starvation case in section 3 rather than a realistic one.
template <typename Reader>
[[nodiscard]] ReadStats hammer(Reader&& reader, std::uint64_t reads, int writer_pause) {
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};

    std::thread writer([&] {
        std::uint64_t v = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            g_lock.write(v++);
            for (int i = 0; i < writer_pause; ++i) _mm_pause();
        }
    });

    Payload p{};
    const auto t0 = Clock::now();
    for (std::uint64_t i = 0; i < reads; ++i) {
        reader(p);
        if (!p.consistent()) torn.fetch_add(1, std::memory_order_relaxed);
    }
    const auto t1 = Clock::now();
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    return { torn.load(), g_retries.exchange(0, std::memory_order_relaxed),
             std::chrono::duration<double, std::nano>(t1 - t0).count()
                 / static_cast<double>(reads) };
}

}  // namespace

int main() {
    constexpr std::uint64_t kReads = 2'000'000;

    std::printf("1. Does the validation actually catch anything?\n");
    std::printf("   One writer updating %zu words; %llu reads; writer paused briefly\n",
                kWords, static_cast<unsigned long long>(kReads));
    std::printf("   between updates so the reader can make progress.\n\n");
    constexpr int kRealisticPause = 200;
    {
        const auto raw = hammer([](Payload& out) {
            Payload tmp{};
            (void)g_lock.try_read(tmp);
            out = tmp;                      // keeps the result even when try_read said no
        }, kReads, kRealisticPause);

        const auto ok = hammer([](Payload& out) { g_lock.read(out, &g_retries); },
                               kReads, kRealisticPause);

        std::printf("   %-42s %12s %10s %10s\n", "reader", "torn reads", "retries", "ns/read");
        std::printf("   %-42s %12llu %10s %10.1f\n", "ignores the seq check (control)",
                    static_cast<unsigned long long>(raw.torn), "-", raw.ns_per_read);
        std::printf("   %-42s %12llu %10llu %10.1f\n", "full seqlock protocol",
                    static_cast<unsigned long long>(ok.torn),
                    static_cast<unsigned long long>(ok.retries), ok.ns_per_read);
        std::printf("\n   The control's torn count is how often the writer was inside the\n");
        std::printf("   payload while the reader read it. The protocol must show zero;\n");
        std::printf("   anything else is a bug in the fences.\n");
    }

    std::printf("\n2. Cost against a mutex, same writer rate\n");
    {
        MutexProtected mp;
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            std::uint64_t v = 1;
            while (!stop.load(std::memory_order_relaxed)) {
                mp.write(v++);
                for (int i = 0; i < kRealisticPause; ++i) _mm_pause();
            }
        });
        Payload p{};
        const auto t0 = Clock::now();
        for (std::uint64_t i = 0; i < kReads; ++i) mp.read(p);
        const auto t1 = Clock::now();
        stop.store(true, std::memory_order_relaxed);
        writer.join();
        const double mutex_ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(kReads);

        const auto sl = hammer([](Payload& out) { g_lock.read(out, &g_retries); },
                               kReads, kRealisticPause);
        std::printf("   %-42s %10.1f ns/read\n", "std::mutex", mutex_ns);
        std::printf("   %-42s %10.1f ns/read\n", "seqlock", sl.ns_per_read);
        if (sl.ns_per_read > 0.0)
            std::printf("   %.1fx, and the seqlock reader never blocks the writer, which is\n",
                        mutex_ns / sl.ns_per_read);
        std::printf("   the property that actually matters on a feed handler.\n");
    }

    std::printf("\n3. The catch: a seqlock guarantees the WRITER progress, not the reader\n");
    {
        const auto starved = hammer([](Payload& out) { g_lock.read(out, &g_retries); },
                                    kReads / 20, 0);   // writer never pauses
        std::printf("   writer paused %-4d spins: reader is fine (section 1 above)\n",
                    kRealisticPause);
        std::printf("   writer paused %-4d spins: %.0f ns/read, %llu retries for %llu reads\n",
                    0, starved.ns_per_read,
                    static_cast<unsigned long long>(starved.retries),
                    static_cast<unsigned long long>(kReads / 20));
        if (starved.retries > 0)
            std::printf("   that is %.0f failed attempts per successful read.\n",
                        static_cast<double>(starved.retries)
                            / static_cast<double>(kReads / 20));
        std::printf("\n   A writer that never lets go starves readers indefinitely: there is\n");
        std::printf("   no queue and no fairness. Readers still return CORRECT data or none,\n");
        std::printf("   never wrong data — but 'eventually' is doing real work in that\n");
        std::printf("   sentence. A mutex would have throttled the writer instead.\n");
        std::printf("   This is the same effect F1 measures as its reader giving up.\n");
    }

    std::printf("\n4. What the fences cost on x86-64\n");
    show("fence_release_only", "// release fence");
    show("fence_acquire_only", "// acquire fence");
    show("fence_seq_cst_only", "// seq_cst fence, for contrast");
    std::printf("   Both fences the seqlock needs emit NOTHING. They exist to stop the\n");
    std::printf("   COMPILER reordering, and on a weaker architecture they would also stop\n");
    std::printf("   the CPU. Omitting them costs nothing here and breaks on ARM — exactly\n");
    std::printf("   the trap J2 showed for release vs relaxed stores.\n");

    std::printf("\n5. Why the payload must be trivially copyable\n");
    std::printf("   A reader copies bytes that may be mid-update. If the payload holds a\n");
    std::printf("   std::vector or std::string, a torn read can capture a stale pointer\n");
    std::printf("   with a new size, or a pointer to a buffer already freed by a realloc.\n");
    std::printf("   The reader then dereferences it. That is a crash, not a retry.\n");
    std::printf("   Payload here is %zu bytes of std::uint64_t: is_trivially_copyable = %s\n",
                sizeof(Payload), std::is_trivially_copyable_v<Payload> ? "true" : "false");
    std::printf("   Per-word relaxed atomics keep it race-free, and cost nothing: the\n");
    std::printf("   generated loads and stores are the same mov instructions.\n");

    std::printf("\n6. Audit: the seqlock already in F1_feed_handling.cpp (FeedD)\n");
    std::printf("   Four real defects, none of which show up on x86:\n\n");
    std::printf("   a) The writer's first store uses memory_order_release:\n");
    std::printf("        seq_.store(seq_+1, release);   // make it odd\n");
    std::printf("        book_.bids[...] = ...;         // may be hoisted ABOVE the store\n");
    std::printf("      A release store orders what precedes it. Preventing the data writes\n");
    std::printf("      from moving above the odd marker needs a release FENCE after it.\n\n");
    std::printf("   b) The reader's second load uses memory_order_acquire:\n");
    std::printf("        out = book_;                   // may sink BELOW the load\n");
    std::printf("        s2 = seq_.load(acquire);\n");
    std::printf("      An acquire load orders what follows it. The data reads need an\n");
    std::printf("      acquire FENCE before s2 is read.\n\n");
    std::printf("   c) `out = book_` copies a std::vector while the writer mutates it.\n");
    std::printf("      That is a data race on a non-atomic object: undefined behaviour,\n");
    std::printf("      and if resize() is ever called concurrently the vector's pointer\n");
    std::printf("      and size can tear, turning a retry into a segfault.\n\n");
    std::printf("   d) `if (s1 & 1) continue;` burns one of only 16 attempts every time it\n");
    std::printf("      finds the writer busy, instead of spinning until it is free. That is\n");
    std::printf("      why F1 reports the reader giving up on most attempts: with a writer\n");
    std::printf("      running flat out, 16 tries are exhausted almost immediately.\n");
    std::printf("      Measured in F1: 1827 consistent snapshots against 6414 give-ups.\n");
}
