// I3 — page boundary crossing: the comb-shaped latency profile.
//
// A TLB entry covers one whole page (4 KB by default). Walk within a page and
// translation is free; step across a 4 KB boundary into a page whose translation
// is not cached and the core must walk the page table. Plot latency against
// address and you get a comb: a flat baseline with a periodic spike.
//
// This file measures that comb directly: it times every individual access with
// RDTSC and averages by OFFSET WITHIN THE PAGE, so offset 0 (the first line of
// each new page) is separated from offsets 64..4032.
//
// There is a catch worth knowing, and §4 quantifies it. The first line of a page
// is slow for TWO independent reasons:
//
//   1. the TLB miss and page walk, and
//   2. Intel's L2 streaming prefetcher does not cross a 4 KB boundary either, so
//      that line is also the one line in the page nobody prefetched.
//
// Those are easy to conflate and blame entirely on the TLB. They can be
// separated: run the identical access pattern on 2 MB pages. The prefetcher
// still restarts every 4 KB, but there is no longer a translation to miss, so
// whatever spike remains is prefetch-only, and the difference is the real TLB
// component.
//
// Pages are visited in random order so the TLB cannot prefetch translations;
// lines WITHIN each page are visited sequentially so the baseline stays flat.
// §5 shows how much of the comb survives when pages go in order instead.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

namespace {

constexpr std::size_t kLine       = 64;
constexpr std::size_t kPageSize   = 4096;
constexpr std::size_t kLinesPerPage = kPageSize / kLine;    // 64 buckets
constexpr std::size_t kPages      = 16384;                  // 64 MB, well past TLB reach

// --- hardware counter, this process only ----------------------------------
class PerfCounter {
public:
    PerfCounter(std::uint32_t type, std::uint64_t config) {
        perf_event_attr attr{};
        attr.size = sizeof(attr);
        attr.type = type;
        attr.config = config;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
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
        std::uint64_t v = 0;
        return ::read(fd_, &v, sizeof(v)) == sizeof(v) ? v : 0;
    }
private:
    int fd_ = -1;
};

[[nodiscard]] std::uint64_t dtlb_read_miss() {
    return PERF_COUNT_HW_CACHE_DTLB
         | (PERF_COUNT_HW_CACHE_OP_READ << 8)
         | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
}

// --- serialising timestamp ------------------------------------------------
[[gnu::always_inline]] inline std::uint64_t cycles() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

[[nodiscard]] double calibrate_overhead() {
    constexpr int kN = 200000;
    std::vector<std::uint64_t> d(kN);
    for (int i = 0; i < kN; ++i) {
        const auto a = cycles();
        const auto b = cycles();
        d[static_cast<std::size_t>(i)] = b - a;
    }
    std::sort(d.begin(), d.end());
    return static_cast<double>(d[kN / 2]);      // median, robust to interrupts
}

enum class Pages { Small, HugeTlb };

[[nodiscard]] char* map_region(std::size_t bytes, Pages kind) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (kind == Pages::HugeTlb) flags |= MAP_HUGETLB;
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (kind == Pages::Small) ::madvise(p, bytes, MADV_NOHUGEPAGE);
    std::memset(p, 0, bytes);
    return static_cast<char*>(p);
}

struct Profile {
    std::array<double, kLinesPerPage> cycles_at_offset{};
    std::uint64_t dtlb_misses = 0;
    bool ok = false;
};

// Builds a chain that is sequential inside each page and jumps between pages in
// `order`, then times every step and buckets it by offset within the page.
[[nodiscard]] Profile measure_comb(Pages kind, bool random_page_order, double overhead) {
    const std::size_t bytes = kPages * kPageSize;
    char* base = map_region(bytes, kind);
    if (!base) return {};

    std::vector<std::uint64_t> order(kPages);
    std::iota(order.begin(), order.end(), 0);
    if (random_page_order) {
        std::mt19937_64 rng{999};
        std::shuffle(order.begin(), order.end(), rng);
    }

    // Link: page order[i] line 0 -> line 1 -> ... -> line 63 -> page order[i+1] line 0.
    // Values stored are absolute byte offsets from base.
    for (std::size_t i = 0; i < kPages; ++i) {
        const std::uint64_t page = order[i] * kPageSize;
        for (std::size_t l = 0; l + 1 < kLinesPerPage; ++l)
            *reinterpret_cast<std::uint64_t*>(base + page + l * kLine) = page + (l + 1) * kLine;
        const std::uint64_t next_page = order[(i + 1) % kPages] * kPageSize;
        *reinterpret_cast<std::uint64_t*>(base + page + (kLinesPerPage - 1) * kLine) = next_page;
    }

    std::array<std::uint64_t, kLinesPerPage> sum{};
    std::array<std::uint64_t, kLinesPerPage> count{};

    // Warm the chain once so the data is resident; translations for 64 MB of
    // 4 KB pages cannot all stay in a 1024-entry TLB, which is the point.
    std::uint64_t idx = order[0] * kPageSize;
    for (std::size_t s = 0; s < kPages * kLinesPerPage; ++s)
        idx = *reinterpret_cast<const std::uint64_t*>(base + idx);

    PerfCounter dtlb(PERF_TYPE_HW_CACHE, dtlb_read_miss());
    dtlb.start();
    for (std::size_t s = 0; s < kPages * kLinesPerPage; ++s) {
        const std::size_t bucket = (idx % kPageSize) / kLine;
        const auto t0 = cycles();
        idx = *reinterpret_cast<const std::uint64_t*>(base + idx);
        const auto t1 = cycles();
        sum[bucket] += (t1 - t0);
        ++count[bucket];
    }
    Profile p;
    p.dtlb_misses = dtlb.stop();
    for (std::size_t b = 0; b < kLinesPerPage; ++b) {
        const double mean = count[b] ? static_cast<double>(sum[b]) / static_cast<double>(count[b]) : 0.0;
        p.cycles_at_offset[b] = mean - overhead;        // remove the RDTSC cost
        if (p.cycles_at_offset[b] < 0.0) p.cycles_at_offset[b] = 0.0;
    }
    p.ok = true;
    ::munmap(base, bytes);
    return p;
}

// Vertical ASCII plot: 64 columns, one per cache line in the page.
void plot(const Profile& p, int height) {
    const double top = *std::max_element(p.cycles_at_offset.begin(), p.cycles_at_offset.end());
    if (top <= 0.0) return;
    std::printf("   cycles\n");
    for (int row = height; row >= 1; --row) {
        const double level = top * row / height;
        std::printf("   %6.0f |", level);
        for (std::size_t b = 0; b < kLinesPerPage; ++b)
            std::putchar(p.cycles_at_offset[b] >= level ? '#' : ' ');
        std::putchar('\n');
    }
    std::printf("          +");
    for (std::size_t b = 0; b < kLinesPerPage; ++b) std::putchar('-');
    std::printf("\n           0");
    for (int i = 0; i < 54; ++i) std::putchar(' ');
    std::printf("4KB\n           offset within page (one column per 64-byte line)\n");
}

struct Summary { double first_line, rest_mean, ratio; };

[[nodiscard]] Summary summarise(const Profile& p) {
    const double first = p.cycles_at_offset[0];
    double acc = 0.0;
    for (std::size_t b = 1; b < kLinesPerPage; ++b) acc += p.cycles_at_offset[b];
    const double rest = acc / static_cast<double>(kLinesPerPage - 1);
    return { first, rest, rest > 0.0 ? first / rest : 0.0 };
}

}  // namespace

int main() {
    const double overhead = calibrate_overhead();
    std::printf("RDTSC+lfence overhead: %.0f cycles (subtracted from every sample)\n", overhead);
    std::printf("region: %zu pages = %zu MB, %zu samples per run\n\n",
                kPages, kPages * kPageSize / (1024 * 1024), kPages * kLinesPerPage);

    std::printf("1. 4 KB pages, pages visited in RANDOM order\n");
    const auto small = measure_comb(Pages::Small, true, overhead);
    if (!small.ok) { std::printf("   mapping failed\n"); return 1; }
    plot(small, 12);
    const auto s4 = summarise(small);
    std::printf("\n   offset 0 (first line of each page): %7.1f cycles\n", s4.first_line);
    std::printf("   offsets 64..4032 (mean)           : %7.1f cycles\n", s4.rest_mean);
    std::printf("   spike                             : %7.2fx\n", s4.ratio);
    std::printf("   dTLB load misses: %llu over %zu accesses (%.1f%%)\n",
                static_cast<unsigned long long>(small.dtlb_misses),
                kPages * kLinesPerPage,
                100.0 * static_cast<double>(small.dtlb_misses)
                      / static_cast<double>(kPages * kLinesPerPage));

    std::printf("\n2. Same pattern on 2 MB pages — the 4 KB translation boundary is gone\n");
    const auto huge = measure_comb(Pages::HugeTlb, true, overhead);
    if (!huge.ok) {
        std::printf("   MAP_HUGETLB unavailable; skipping\n");
    } else {
        plot(huge, 12);
        const auto s2 = summarise(huge);
        std::printf("\n   offset 0: %7.1f cycles, offsets 64..4032: %7.1f cycles, spike %.2fx\n",
                    s2.first_line, s2.rest_mean, s2.ratio);
        std::printf("   dTLB load misses: %llu (%.1f%%)\n",
                    static_cast<unsigned long long>(huge.dtlb_misses),
                    100.0 * static_cast<double>(huge.dtlb_misses)
                          / static_cast<double>(kPages * kLinesPerPage));

        std::printf("\n3. Decomposing the spike at offset 0\n");
        const double excess_4k = s4.first_line - s4.rest_mean;
        const double excess_2m = s2.first_line - s2.rest_mean;
        std::printf("   excess on 4 KB pages : %6.1f cycles  (TLB miss + prefetch restart)\n", excess_4k);
        std::printf("   excess on 2 MB pages : %6.1f cycles  (prefetch restart only)\n", excess_2m);
        std::printf("   attributable to TLB  : %6.1f cycles  (%.0f%% of the 4 KB spike)\n",
                    excess_4k - excess_2m,
                    excess_4k > 0.0 ? 100.0 * (excess_4k - excess_2m) / excess_4k : 0.0);
        std::printf("\n   Both page sizes restart the L2 streamer every 4 KB, so the 2 MB\n");
        std::printf("   row isolates that cost. Only the remainder is translation.\n");
    }

    std::printf("\n4. Pages visited in SEQUENTIAL order instead\n");
    const auto seq = measure_comb(Pages::Small, false, overhead);
    if (seq.ok) {
        plot(seq, 12);
        const auto ss = summarise(seq);
        std::printf("\n   offset 0: %7.1f cycles, offsets 64..4032: %7.1f cycles, spike %.2fx\n",
                    ss.first_line, ss.rest_mean, ss.ratio);
        std::printf("   dTLB load misses: %llu (%.1f%%)\n",
                    static_cast<unsigned long long>(seq.dtlb_misses),
                    100.0 * static_cast<double>(seq.dtlb_misses)
                          / static_cast<double>(kPages * kLinesPerPage));
        std::printf("\n   Sequential pages are much cheaper: 512 page-table entries share\n");
        std::printf("   one 4 KB page of PTEs, so consecutive walks hit cache, and the\n");
        std::printf("   hardware can prefetch the next translation. The comb is still\n");
        std::printf("   there, but far shallower — which is why a linear scan of a huge\n");
        std::printf("   array is not usually TLB-bound, while a pointer-chasing structure\n");
        std::printf("   over the same bytes is.\n");
    }

    std::printf("\n5. Mitigations, in the order worth trying\n");
    std::printf("   - huge pages: MADV_HUGEPAGE or MAP_HUGETLB moves the boundary to 2 MB\n");
    std::printf("   - visit memory in address order where you can; random page order is\n");
    std::printf("     what turns a shallow comb into a deep one\n");
    std::printf("   - pack hot fields so one page holds more useful data\n");
    std::printf("   - do not straddle the boundary with a single access (see I2 section 3)\n");
}
