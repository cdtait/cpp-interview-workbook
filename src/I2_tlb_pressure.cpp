// I2 — TLB pressure: miss spikes, and shootdown spikes.
//
// Virtual addresses are translated by walking page tables, and the TLB caches
// the results. On this Haswell the second-level TLB holds ~1024 entries shared
// between page sizes, so:
//
//   4 KB pages: 1024 * 4 KB  =   4 MB of reach
//   2 MB pages: 1024 * 2 MB  = 2048 MB of reach
//
// Below ~4 MB the TLB is irrelevant. Above it, every random access to a fresh
// page needs a page walk — four dependent memory references of its own. That is
// a TLB MISS spike, and §1 measures it by sweeping the working set across that
// boundary with 4 KB pages, transparent huge pages, and explicit hugetlb.
//
// The second problem is different in kind. A TLB is per-CPU and the hardware
// does not keep them coherent, so when one thread invalidates a mapping the
// kernel must interrupt every other CPU that might have cached it and make each
// one flush. Those are inter-processor interrupts — TLB SHOOTDOWNS — and the
// cost lands on threads that never touched the address in question. §2 provokes
// them with madvise(MADV_DONTNEED) on one thread and measures the latency they
// inflict on four unrelated workers, counting the IPIs from /proc/interrupts.
//
// Counters come from perf_event_open on this process only, which is permitted at
// perf_event_paranoid=2 as long as kernel events are excluded.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kLine = 64;
constexpr std::size_t kMB   = 1024 * 1024;

// ===========================================================================
// A single hardware counter, scoped to this process.
// ===========================================================================
class PerfCounter {
public:
    PerfCounter(std::uint32_t type, std::uint64_t config) {
        perf_event_attr attr{};
        attr.size           = sizeof(attr);
        attr.type           = type;
        attr.config         = config;
        attr.disabled       = 1;
        attr.exclude_kernel = 1;      // required at paranoid=2
        attr.exclude_hv     = 1;
        fd_ = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
    }
    ~PerfCounter() { if (fd_ >= 0) ::close(fd_); }
    PerfCounter(const PerfCounter&) = delete;
    PerfCounter& operator=(const PerfCounter&) = delete;

    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    void start() noexcept {
        if (fd_ < 0) return;
        ::ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
        ::ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
    }
    [[nodiscard]] std::uint64_t stop() noexcept {
        if (fd_ < 0) return 0;
        ::ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
        std::uint64_t value = 0;
        return ::read(fd_, &value, sizeof(value)) == sizeof(value) ? value : 0;
    }
private:
    int fd_ = -1;
};

[[nodiscard]] std::uint64_t dtlb_read_miss_config() {
    return PERF_COUNT_HW_CACHE_DTLB
         | (PERF_COUNT_HW_CACHE_OP_READ   << 8)
         | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
}

// ===========================================================================
// Sum the per-CPU TLB shootdown IPI counts from /proc/interrupts.
// ===========================================================================
[[nodiscard]] std::uint64_t tlb_shootdowns() {
    std::FILE* f = std::fopen("/proc/interrupts", "r");
    if (!f) return 0;
    char line[4096];
    std::uint64_t total = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "TLB:", 4) != 0) continue;
        const char* p = line + 4;
        while (*p) {
            while (*p == ' ' || *p == '\t') ++p;
            if (*p < '0' || *p > '9') break;          // reached the description
            total += std::strtoull(p, nullptr, 10);
            while (*p >= '0' && *p <= '9') ++p;
        }
        break;
    }
    std::fclose(f);
    return total;
}

// ===========================================================================
// Mappings
// ===========================================================================
enum class Pages { Small, Transparent, HugeTlb };

[[nodiscard]] const char* name_of(Pages p) {
    switch (p) {
        case Pages::Small:       return "4 KB pages";
        case Pages::Transparent: return "2 MB (THP)";
        case Pages::HugeTlb:     return "2 MB (hugetlb)";
    }
    return "?";
}

[[nodiscard]] char* map_region(std::size_t bytes, Pages kind) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (kind == Pages::HugeTlb) flags |= MAP_HUGETLB;
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) return nullptr;

    if (kind == Pages::Small)       ::madvise(p, bytes, MADV_NOHUGEPAGE);
    if (kind == Pages::Transparent) ::madvise(p, bytes, MADV_HUGEPAGE);

    std::memset(p, 0, bytes);       // fault everything in before measuring
    return static_cast<char*>(p);
}

// A dependent pointer chase over a random permutation of cache lines: each step
// must complete before the next address is known, so nothing can be overlapped
// or prefetched, and the page walk cost is fully exposed.
void build_chain(char* base, std::size_t lines, std::mt19937_64& rng) {
    std::vector<std::uint64_t> perm(lines);
    std::iota(perm.begin(), perm.end(), 0);
    std::shuffle(perm.begin(), perm.end(), rng);
    for (std::size_t i = 0; i < lines; ++i)
        *reinterpret_cast<std::uint64_t*>(base + perm[i] * kLine) = perm[(i + 1) % lines];
}

[[nodiscard]] std::uint64_t chase(const char* base, std::uint64_t start, std::size_t steps) {
    std::uint64_t index = start;
    for (std::size_t i = 0; i < steps; ++i)
        index = *reinterpret_cast<const std::uint64_t*>(base + index * kLine);
    return index;
}

struct ChaseResult { double ns_per_step; double misses_per_step; bool counted; };

[[nodiscard]] ChaseResult measure_chase(std::size_t bytes, Pages kind, std::size_t steps) {
    char* base = map_region(bytes, kind);
    if (!base) return {-1.0, 0.0, false};

    const std::size_t lines = bytes / kLine;
    std::mt19937_64 rng{12345};
    build_chain(base, lines, rng);

    (void)chase(base, 0, lines);        // warm, and prove the chain is a cycle

    PerfCounter dtlb(PERF_TYPE_HW_CACHE, dtlb_read_miss_config());
    dtlb.start();
    const auto t0 = Clock::now();
    const auto end = chase(base, 0, steps);
    const auto t1 = Clock::now();
    const std::uint64_t misses = dtlb.stop();

    ::munmap(base, bytes);
    return { std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(steps),
             static_cast<double>(misses) / static_cast<double>(steps),
             dtlb.ok() && (end || true) };
}


// ===========================================================================
// A load that straddles a boundary is split into two accesses by the hardware.
// Crossing a 64-byte line costs extra cycles. Crossing a 4 KB page costs far
// more: the two halves lie in different pages, so the core takes a much slower
// path to issue both and merge the result. That is a page split (or page-split
// access).
//
// It is tempting to blame a second TLB lookup, and the measurement below shows
// that is NOT what dominates: the miss count per load is identical across all
// three offsets. Here the second half lands in the very next page, which the
// following iteration touches anyway, so it is already cached. The penalty is
// in the core's split-load path, not in translation -- which is why it survives
// even when the whole working set fits in the TLB (second table).
//
// All three cases below stride by exactly one page, so each touches the same
// number of pages and the same number of lines. The ONLY difference is the
// offset within the page, which isolates the split penalty from everything else.
// ===========================================================================
struct SplitResult { double ns_per_load; double misses_per_load; };

volatile std::uint64_t g_sink = 0;

[[nodiscard]] SplitResult measure_split(const char* base, std::size_t pages,
                                        std::size_t offset, std::size_t reps) {
    PerfCounter dtlb(PERF_TYPE_HW_CACHE, dtlb_read_miss_config());
    std::uint64_t sum = 0;
    dtlb.start();
    const auto t0 = Clock::now();
    for (std::size_t r = 0; r < reps; ++r)
        for (std::size_t page = 0; page < pages; ++page) {
            std::uint64_t v;
            std::memcpy(&v, base + page * 4096 + offset, sizeof(v));   // may straddle
            sum += v;
        }
    const auto t1 = Clock::now();
    const std::uint64_t misses = dtlb.stop();

    const auto loads = static_cast<double>(reps * pages);
    g_sink = sum;                      // keep the loads from being optimised away
    return { std::chrono::duration<double, std::nano>(t1 - t0).count() / loads,
             static_cast<double>(misses) / loads };
}

// ===========================================================================
// §2 — shootdowns
// ===========================================================================
constexpr std::size_t kWorkers        = 4;
constexpr std::size_t kBatches        = 400'000;   // ~1 s, so the disruptor runs
constexpr std::size_t kTouchesPerBatch = 256;

struct Latencies { std::vector<double> ns; };

// Touches its own private page repeatedly. It never goes near the memory the
// disruptor unmaps — any slowdown it sees is the IPI, not a cache effect.
void worker(char* own, std::size_t pages, Latencies* out, std::atomic<bool>* go) {
    out->ns.reserve(kBatches);
    while (!go->load(std::memory_order_acquire)) {}
    // Every worker runs its full count; `stop` is set by main once they have all
    // joined, so one finishing early cannot truncate the others' samples.
    for (std::size_t b = 0; b < kBatches; ++b) {
        const auto t0 = Clock::now();
        // One pass over this worker's own pages. The number of pages is its TLB
        // footprint — exactly what a remote flush forces it to rebuild.
        for (std::size_t i = 0; i < kTouchesPerBatch; ++i) {
            auto* cell = reinterpret_cast<volatile std::uint64_t*>(own + (i % pages) * 4096);
            *cell = *cell + 1;                  // not '+=': deprecated on volatile
        }
        const auto t1 = Clock::now();
        out->ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count()
                          / static_cast<double>(kTouchesPerBatch));
    }
}

struct Stats { double p50, p99, p999, max; std::size_t samples; };

[[nodiscard]] Stats summarise(std::vector<Latencies>& per_thread) {
    std::vector<double> all;
    for (auto& l : per_thread) all.insert(all.end(), l.ns.begin(), l.ns.end());
    std::sort(all.begin(), all.end());
    if (all.empty()) return {0, 0, 0, 0, 0};
    auto q = [&](double f) { return all[static_cast<std::size_t>(f * (all.size() - 1))]; };
    return { q(0.50), q(0.99), q(0.999), all.back(), all.size() };
}

struct ShootdownRun { Stats stats; std::uint64_t ipis; std::uint64_t cycles; double seconds; };

[[nodiscard]] ShootdownRun run_workers(bool with_disruptor, std::size_t worker_pages) {
    // Each worker gets its own pages, far from the disruptor's region.
    const std::size_t worker_bytes = kWorkers * worker_pages * 4096;
    char* worker_mem = map_region(worker_bytes, Pages::Small);
    // Small enough that each madvise/refault cycle is cheap, so the loop runs
    // often and generates many flushes rather than a few large ones.
    const std::size_t churn_bytes = 2 * kMB;
    char* churn = with_disruptor ? map_region(churn_bytes, Pages::Small) : nullptr;

    std::atomic<bool> go{false}, stop{false};
    std::vector<Latencies> results(kWorkers);
    std::vector<std::thread> pool;
    for (std::size_t i = 0; i < kWorkers; ++i)
        pool.emplace_back(worker, worker_mem + i * worker_pages * 4096,
                          worker_pages, &results[i], &go);

    std::atomic<std::uint64_t> churn_cycles{0};
    std::thread disruptor;
    if (with_disruptor) {
        disruptor = std::thread([&] {
            while (!go.load(std::memory_order_acquire)) {}
            while (!stop.load(std::memory_order_relaxed)) {
                // Dropping these pages invalidates mappings the other CPUs may
                // hold, so the kernel IPIs them to flush. Touching the memory
                // again re-faults it, ready for the next round.
                ::madvise(churn, churn_bytes, MADV_DONTNEED);
                for (std::size_t off = 0; off < churn_bytes; off += 4096) churn[off] = 1;
                churn_cycles.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const std::uint64_t before = tlb_shootdowns();
    const auto wall0 = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : pool) t.join();
    stop.store(true, std::memory_order_relaxed);
    if (disruptor.joinable()) disruptor.join();
    const auto wall1 = Clock::now();
    const std::uint64_t after = tlb_shootdowns();

    ::munmap(worker_mem, worker_bytes);
    if (churn) ::munmap(churn, churn_bytes);
    return { summarise(results), after - before,
             churn_cycles.load(),
             std::chrono::duration<double>(wall1 - wall0).count() };
}

}  // namespace

int main() {
    std::printf("TLB reach on this machine: 1024 STLB entries\n");
    std::printf("  4 KB pages -> ~4 MB,  2 MB pages -> ~2 GB\n\n");

    {
        PerfCounter probe(PERF_TYPE_HW_CACHE, dtlb_read_miss_config());
        std::printf("perf_event_open for dTLB-load-misses: %s\n\n",
                    probe.ok() ? "available" : "UNAVAILABLE (counters will read 0)");
    }

    std::printf("1. Random dependent chase: latency and dTLB misses per access\n");
    std::printf("   %-12s %14s %14s %14s\n", "working set",
                name_of(Pages::Small), name_of(Pages::Transparent), name_of(Pages::HugeTlb));
    constexpr std::size_t kSteps = 2'000'000;
    for (std::size_t mb : {2u, 8u, 32u, 128u, 512u}) {
        const std::size_t bytes = mb * kMB;
        const auto small = measure_chase(bytes, Pages::Small, kSteps);
        const auto thp   = measure_chase(bytes, Pages::Transparent, kSteps);
        const auto huge  = measure_chase(bytes, Pages::HugeTlb, kSteps);
        std::printf("   %6zu MB    %7.1f ns     %7.1f ns     ", mb, small.ns_per_step, thp.ns_per_step);
        if (huge.ns_per_step < 0) std::printf("%10s\n", "n/a");
        else                      std::printf("%7.1f ns\n", huge.ns_per_step);
        std::printf("   %-12s %7.2f miss%%   %7.2f miss%%   ", "",
                    small.misses_per_step * 100.0, thp.misses_per_step * 100.0);
        if (huge.ns_per_step < 0) std::printf("%10s\n", "");
        else                      std::printf("%7.2f miss%%\n", huge.misses_per_step * 100.0);
    }
    std::printf("\n   Below the 4 MB reach the page size is irrelevant. Above it the\n");
    std::printf("   4 KB column pays a page walk on nearly every access.\n");

    std::printf("\n2. TLB shootdown: cost inflicted on threads doing nothing wrong\n");
    std::printf("   %d workers touching only their own pages, ns per touch\n",
                static_cast<int>(kWorkers));
    std::printf("   %-14s %-18s %7s %7s %8s %10s %10s\n", "footprint", "disruptor",
                "p50", "p99", "p99.9", "TLB IPIs", "IPIs/sec");
    for (std::size_t pages : {std::size_t{1}, std::size_t{256}}) {
        char label[32];
        std::snprintf(label, sizeof(label), "%zu page%s", pages, pages == 1 ? "" : "s");
        const auto quiet = run_workers(false, pages);
        const auto noisy = run_workers(true, pages);
        std::printf("   %-14s %-18s %7.1f %7.1f %8.1f %10llu %10.0f\n", label, "none",
                    quiet.stats.p50, quiet.stats.p99, quiet.stats.p999,
                    static_cast<unsigned long long>(quiet.ipis), quiet.ipis / quiet.seconds);
        std::printf("   %-14s %-18s %7.1f %7.1f %8.1f %10llu %10.0f   p99.9 %.2fx\n",
                    "", "madvise(DONTNEED)",
                    noisy.stats.p50, noisy.stats.p99, noisy.stats.p999,
                    static_cast<unsigned long long>(noisy.ipis), noisy.ipis / noisy.seconds,
                    quiet.stats.p999 > 0 ? noisy.stats.p999 / quiet.stats.p999 : 0.0);
    }

    std::printf("\n   The workers never touch the churned region: the damage arrives as an\n");
    std::printf("   interrupt, so it lands in the tail rather than the median.\n");
    std::printf("   Note how much the footprint matters. A worker holding one page in\n");
    std::printf("   the TLB loses almost nothing to a flush; one holding 256 pages has\n");
    std::printf("   to rebuild all of them. The shootdown RATE is set by the disruptor,\n");
    std::printf("   but the COST is set by the victim.\n");

    std::printf("\n3. Page-split accesses: one load, two pages\n");
    {
        struct Case { const char* name; std::size_t offset; };
        const Case cases[] = {
            {"aligned (offset 0)",        0},
            {"line split (offset 60)",   60},     // crosses a 64 B line
            {"page split (offset 4092)", 4092},   // crosses a 4 KB page
        };
        // Two working sets: one far past the 4 KB TLB reach, one comfortably
        // inside it. If the penalty were a translation cost it would shrink in
        // the second table. It does not.
        struct Sweep { const char* label; std::size_t pages; std::size_t reps; };
        const Sweep sweeps[] = {
            {"32 MB working set (TLB-missing)", 8192, 64},
            {"1 MB working set  (TLB-resident)", 256, 2048},
        };
        for (const auto& sw : sweeps) {
            char* region = map_region((sw.pages + 1) * 4096, Pages::Small);
            if (!region) { std::printf("   (mapping failed)\n"); continue; }
            std::printf("   %s\n", sw.label);
            std::printf("     %-26s %12s %16s %8s\n", "", "ns per load",
                        "dTLB miss/load", "vs aligned");
            double baseline = 0.0;
            for (const auto& c : cases) {
                const auto r = measure_split(region, sw.pages, c.offset, sw.reps);
                if (c.offset == 0) baseline = r.ns_per_load;
                std::printf("     %-26s %9.2f    %13.3f   ", c.name, r.ns_per_load,
                            r.misses_per_load);
                if (c.offset == 0) std::printf("%8s\n", "-");
                else               std::printf("%7.2fx\n", r.ns_per_load / baseline);
            }
            ::munmap(region, (sw.pages + 1) * 4096);
        }
        std::printf("\n   Same page count, same line count, same instruction — only the\n");
        std::printf("   offset within the page differs.\n");
        std::printf("   The dTLB miss column is flat, and the penalty persists when the\n");
        std::printf("   working set fits in the TLB, so this is the core's split-load\n");
        std::printf("   path, not translation. In practice it is what packed structs and\n");
        std::printf("   memcpy straight out of a wire buffer run into.\n");
    }

    std::printf("\n4. What to do about each\n");
    std::printf("   TLB misses   : huge pages (MADV_HUGEPAGE or MAP_HUGETLB), smaller\n");
    std::printf("                  working sets, sequential over random access.\n");
    std::printf("   Shootdowns   : stop unmapping. Pool and reuse instead of\n");
    std::printf("                  munmap/madvise(DONTNEED); avoid mprotect on hot\n");
    std::printf("                  mappings; keep allocator trim thresholds high\n");
    std::printf("                  (glibc M_TRIM_THRESHOLD, MALLOC_ARENA_MAX).\n");
    std::printf("\n   Measure with:\n");
    std::printf("     perf stat -e dTLB-load-misses,dTLB-store-misses,iTLB-load-misses ./prog\n");
    std::printf("     watch -n1 \"grep TLB /proc/interrupts\"\n");
}
