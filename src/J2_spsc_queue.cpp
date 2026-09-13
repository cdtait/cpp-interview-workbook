// J2 — SPSC ring buffer: acquire/release in anger.
//
// A single-producer single-consumer queue is the smallest useful thing you can
// build out of atomics, and it needs exactly two orderings:
//
//   producer:  write the slot;  write_.store(next, RELEASE)
//   consumer:  write_.load(ACQUIRE);  read the slot
//
// The release store publishes everything the producer wrote BEFORE it; the
// acquire load makes those writes visible to whoever reads the index after. No
// locks, no fences you can see on x86, and nothing stronger is required.
//
// Three things this file shows that a textbook usually does not:
//
//   1. seq_cst costs real throughput here, and buys nothing (§1). On x86 the
//      only difference is an xchg on the index store — which, in a loop that
//      runs tens of millions of times, is not free.
//   2. The difference between release and relaxed is INVISIBLE on x86 (§3):
//      both compile to the same two movs, because x86 already orders
//      store-store. On aarch64 release is `stlr` and relaxed is plain `str`.
//      Code that uses relaxed here passes every test on your desktop and
//      corrupts data on an ARM server. That is the whole argument for writing
//      the ordering you mean rather than the ordering that happens to work.
//   3. Where the time actually goes: the index layout matters more than the
//      ordering. head_ and tail_ on one cache line is the I1 false-sharing
//      problem, and caching the other side's index removes most of what remains.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kCapacity = 1024;
constexpr std::uint64_t kItems  = 10'000'000;

struct Item {
    std::uint64_t seq;
    std::uint64_t payload;
};

// Cheap reversible mix, so the consumer can prove it got the right bytes and
// not just a plausible-looking index.
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t v) {
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 33;
    return v;
}

enum class Model { SeqCst, AcqRel };

template <typename T, std::size_t N, Model M, bool Padded, bool Cached>
class Spsc {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

    // The whole point of the file: these are the only orderings involved.
    static constexpr auto kAcq = (M == Model::SeqCst) ? std::memory_order_seq_cst
                                                      : std::memory_order_acquire;
    static constexpr auto kRel = (M == Model::SeqCst) ? std::memory_order_seq_cst
                                                      : std::memory_order_release;
    // An index is only ever written by its owning thread, so that thread can
    // read its own copy with no ordering at all.
    static constexpr auto kOwn = (M == Model::SeqCst) ? std::memory_order_seq_cst
                                                      : std::memory_order_relaxed;

    static constexpr std::size_t kAlign = Padded ? 64 : alignof(std::size_t);

public:
    [[nodiscard]] bool push(const T& value) noexcept {
        const std::size_t w    = producer_.index.load(kOwn);
        const std::size_t next = (w + 1) & (N - 1);

        if constexpr (Cached) {
            if (next == producer_.cached) {                 // may be stale: re-check
                producer_.cached = consumer_.index.load(kAcq);
                if (next == producer_.cached) return false; // genuinely full
            }
        } else {
            if (next == consumer_.index.load(kAcq)) return false;
        }

        buffer_[w] = value;                   // must not be seen after the index moves
        producer_.index.store(next, kRel);    // ... which is exactly what release means
        return true;
    }

    [[nodiscard]] bool pop(T& out) noexcept {
        const std::size_t r = consumer_.index.load(kOwn);

        if constexpr (Cached) {
            if (r == consumer_.cached) {
                consumer_.cached = producer_.index.load(kAcq);
                if (r == consumer_.cached) return false;   // genuinely empty
            }
        } else {
            if (r == producer_.index.load(kAcq)) return false;
        }

        out = buffer_[r];
        consumer_.index.store((r + 1) & (N - 1), kRel);
        return true;
    }

private:
    // Each side's atomic index and its private cache of the OTHER side's index
    // must live on the SAME line: they are both touched by the same thread on
    // every operation. Giving the cache its own line (the obvious reading of
    // "pad everything") makes the producer touch two lines per push and is
    // measurably slower than not caching at all.
    struct alignas(kAlign) Side {
        std::atomic<std::size_t> index{0};   // this side writes it
        std::size_t              cached{0};  // this side's private copy of the other
    };

    std::array<T, N> buffer_{};
    Side producer_;
    Side consumer_;
};

void pin_self(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

std::atomic<std::uint64_t> g_consumer_sink{0};

struct RunResult { double ns_per_item; bool valid; };

// consumer_work simulates a consumer that actually does something per item. With
// it at 0 the consumer outruns the producer and the queue sits empty, which is
// the worst case for index caching: the cached value is always stale, so every
// pop refreshes it AND pays an extra branch. Give the consumer some work and a
// backlog forms, which is the case caching exists for.
template <typename Queue>
[[nodiscard]] RunResult run(std::uint64_t items, std::uint32_t consumer_work = 0) {
    static Queue queue;                 // static: 1024 Items is too big for a stack frame
    std::atomic<bool> ok{true};

    const auto t0 = Clock::now();
    std::thread producer([&] {
        pin_self(0);
        for (std::uint64_t i = 0; i < items; ++i) {
            const Item item{i, mix(i)};
            while (!queue.push(item)) { /* full: spin */ }
        }
    });
    std::thread consumer([&] {
        pin_self(1);
        Item item{};
        std::uint64_t sink = 0;
        for (std::uint64_t i = 0; i < items; ++i) {
            while (!queue.pop(item)) { /* empty: spin */ }
            // Proves ordering, not just delivery: if the slot write were visible
            // after the index write, this would read a stale or torn Item.
            if (item.seq != i || item.payload != mix(i))
                ok.store(false, std::memory_order_relaxed);
            for (std::uint32_t w = 0; w < consumer_work; ++w)
                sink = mix(sink ^ item.payload);        // dependent, so it cannot be elided
        }
        g_consumer_sink.store(sink, std::memory_order_relaxed);
    });
    producer.join();
    consumer.join();
    const auto t1 = Clock::now();

    return { std::chrono::duration<double, std::nano>(t1 - t0).count()
                 / static_cast<double>(items),
             ok.load() };
}

// --- self-disassembly, as in J1 -------------------------------------------
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
    std::printf("   %s  %s\n", symbol, note);
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

// --- the same publish sequence compiled for another architecture -----------
const char* const kSnippet =
    "void publish_release(int* slot, int* idx, int v) {\n"
    "    *slot = v;\n"
    "    __atomic_store_n(idx, 1, __ATOMIC_RELEASE);\n"
    "}\n"
    "void publish_relaxed(int* slot, int* idx, int v) {\n"
    "    *slot = v;\n"
    "    __atomic_store_n(idx, 1, __ATOMIC_RELAXED);\n"
    "}\n"
    "int consume_acquire(int* slot, int* idx) {\n"
    "    if (__atomic_load_n(idx, __ATOMIC_ACQUIRE)) return *slot;\n"
    "    return -1;\n"
    "}\n";

void show_for_target(const char* path, const char* target) {
    char cmd[PATH_MAX + 256];
    std::snprintf(cmd, sizeof(cmd),
                  "clang --target=%s -O2 -S -o - '%s' 2>/dev/null "
                  "| grep -vE '^\\s*\\.|^$|^//|^#' ", target, path);
    std::FILE* pipe = popen(cmd, "r");
    if (!pipe) return;
    char line[512];
    while (std::fgets(line, sizeof(line), pipe)) std::printf("        %s", line);
    pclose(pipe);
}

}  // namespace

// Concrete instantiations, so push() has a symbol that can be disassembled.
namespace {
using QSeqCst = Spsc<Item, kCapacity, Model::SeqCst, true, false>;
using QAcqRel = Spsc<Item, kCapacity, Model::AcqRel, true, false>;
QSeqCst g_seq;
QAcqRel g_acq;
}  // namespace

extern "C" {
[[gnu::noinline]] bool push_seq_cst(const Item* i) { return g_seq.push(*i); }
[[gnu::noinline]] bool push_acq_rel(const Item* i) { return g_acq.push(*i); }
}

int main() {
    std::printf("SPSC ring buffer, capacity %zu, %llu items, producer on cpu0 / consumer on cpu1\n\n",
                kCapacity, static_cast<unsigned long long>(kItems));

    std::printf("1. What the ordering and the layout are each worth\n");
    std::printf("   %-44s %12s %8s\n", "variant", "ns per item", "valid");
    const auto a = run<Spsc<Item, kCapacity, Model::SeqCst, false, false>>(kItems);
    std::printf("   %-44s %12.2f %8s\n", "seq_cst, indices sharing a line", a.ns_per_item,
                a.valid ? "yes" : "NO");
    const auto b = run<Spsc<Item, kCapacity, Model::AcqRel, false, false>>(kItems);
    std::printf("   %-44s %12.2f %8s\n", "acquire/release, indices sharing a line", b.ns_per_item,
                b.valid ? "yes" : "NO");
    const auto c = run<Spsc<Item, kCapacity, Model::AcqRel, true, false>>(kItems);
    std::printf("   %-44s %12.2f %8s\n", "acquire/release, indices padded apart", c.ns_per_item,
                c.valid ? "yes" : "NO");
    const auto d = run<Spsc<Item, kCapacity, Model::AcqRel, true, true>>(kItems);
    std::printf("   %-44s %12.2f %8s\n", "acquire/release, padded + cached indices", d.ns_per_item,
                d.valid ? "yes" : "NO");
    std::printf("\n   'valid' means every item arrived in order with an intact payload —\n");
    std::printf("   if the slot write were ever visible after the index write, it would fail.\n");
    if (a.ns_per_item > 0.0 && d.ns_per_item > 0.0)
        std::printf("   end to end: %.2fx faster, same queue, same correctness\n",
                    a.ns_per_item / d.ns_per_item);

    const bool caching_helped = d.ns_per_item < c.ns_per_item;
    std::printf("\n   Index caching %s above (%.2f vs %.2f). Its benefit depends on whether\n",
                caching_helped ? "won" : "LOST", d.ns_per_item, c.ns_per_item);
    std::printf("   a backlog exists: with an idle-fast consumer the queue sits empty, the\n");
    std::printf("   cached value is always stale, and every pop refreshes it anyway — paying\n");
    std::printf("   an extra branch for nothing. This run is close to that boundary, so the\n");
    std::printf("   result moves between runs. Same two queues with the consumer given real\n");
    std::printf("   work, so a backlog genuinely forms:\n\n");
    constexpr std::uint64_t kBacklogItems = 2'000'000;
    constexpr std::uint32_t kWork = 12;
    const auto e = run<Spsc<Item, kCapacity, Model::AcqRel, true, false>>(kBacklogItems, kWork);
    const auto f = run<Spsc<Item, kCapacity, Model::AcqRel, true, true>>(kBacklogItems, kWork);
    std::printf("   %-44s %12.2f %8s\n", "padded, NOT cached  (with backlog)", e.ns_per_item,
                e.valid ? "yes" : "NO");
    std::printf("   %-44s %12.2f %8s\n", "padded + cached     (with backlog)", f.ns_per_item,
                f.valid ? "yes" : "NO");
    if (e.ns_per_item > 0.0 && f.ns_per_item > 0.0)
        std::printf("   caching is now %.2fx %s\n", 
                    f.ns_per_item < e.ns_per_item ? e.ns_per_item / f.ns_per_item
                                                  : f.ns_per_item / e.ns_per_item,
                    f.ns_per_item < e.ns_per_item ? "faster" : "STILL slower");

    std::printf("\n2. push(), disassembled from this binary\n");
    show("push_acq_rel", "// release store: a plain mov");
    show("push_seq_cst", "// seq_cst store: xchg, and it is in the hot loop");

    std::printf("\n3. The same publish sequence, two architectures\n");
    char path[] = "/tmp/j2_publish_XXXXXX.c";
    const int fd = mkstemps(path, 2);
    if (fd < 0) {
        std::printf("   (could not write a temp file)\n");
    } else {
        const auto len = std::strlen(kSnippet);
        const bool written = ::write(fd, kSnippet, len) == static_cast<ssize_t>(len);
        ::close(fd);
        if (written) {
            std::printf("     *slot = v;  then  __atomic_store_n(idx, 1, RELEASE or RELAXED)\n\n");
            std::printf("   x86-64:\n");
            show_for_target(path, "x86_64-linux-gnu");
            std::printf("\n   aarch64:\n");
            show_for_target(path, "aarch64-linux-gnu");
            std::printf("\n   On x86 release and relaxed are the SAME instruction, because the\n");
            std::printf("   hardware never reorders store-store. On aarch64 release becomes\n");
            std::printf("   stlr and relaxed stays str — so a queue written with relaxed is\n");
            std::printf("   correct on your desktop and broken on an ARM server, with no test\n");
            std::printf("   on x86 able to tell you. Note ldar for the acquire load too.\n");
            std::printf("   (needs clang; nothing printed above means it is not installed)\n");
        }
        ::unlink(path);
    }

    std::printf("\n4. What actually mattered\n");
    std::printf("   - ordering:  seq_cst -> acquire/release is a real win and costs nothing\n");
    std::printf("     in correctness; the release store is the minimum that is sound.\n");
    std::printf("   - layout:    padding head and tail apart is the I1 false-sharing fix,\n");
    std::printf("     applied to the two indices the two threads write.\n");
    std::printf("   - caching:   keeping a private copy of the other side's index means the\n");
    std::printf("     common case touches only lines this thread already owns. The atomic\n");
    std::printf("     load is only paid when the queue looks full or empty.\n");
}
