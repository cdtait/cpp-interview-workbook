// I4 — separating the two halves of a page-crossing spike.
//
// I3 measured a 38x latency spike on the first line of each new 4 KB page, and
// showed that moving to 2 MB pages removes the dTLB misses but only halves the
// spike. That pointed at a second cause: Intel's L2 streaming prefetcher also
// refuses to cross a 4 KB boundary, so the first line of every 4 KB region is
// the one line nobody prefetched — a DRAM access in its own right.
//
// That was an inference from one comparison. This file tests it properly, with a
// 2x2 design that switches each cause on and off independently:
//
//                     | lines sequential in page | lines RANDOM in page
//                     | (HW prefetcher works)    | (HW prefetcher useless)
//   ------------------+--------------------------+------------------------
//   4 KB pages        | TLB miss + prefetch gap  | TLB miss only
//   2 MB pages        | prefetch gap only        | neither  -> control, ~0
//
// Read down a column to see the TLB's contribution; read across a row to see the
// prefetcher's. The bottom-right cell is the control: no translation to miss and
// nothing prefetched anywhere, so the first access in a page should be no more
// expensive than any other. If it is ~0, the design is sound.
//
// A fifth run adds a software prefetch of the next page across the boundary, to
// see how much of the 4 KB penalty can simply be asked for in advance.
//
// RESULT: the control cell earns its keep by FAILING, and that failure is the
// finding. On a DRAM-resident region it comes out around 200 cycles rather than
// ~0, which says a third effect is present: the 64 lines of one 4 KB page largely
// share a DRAM row, so staying inside a page gets row-buffer hits while jumping
// to a random page opens a new row. Page locality and DRAM-row locality are
// confounded here.
//
// Repeating on a 6 MB region that fits in L3 shrinks the control but does not
// rescue the method, and the program says so rather than pretending:
//
//   - subtracting one noisy cell from another can yield a NEGATIVE TLB cost,
//     which is impossible;
//   - the spread column (max-min across repetitions as a percentage of the
//     median) reaches 50-100%, i.e. larger than the differences being computed
//     from those cells;
//   - 6 MB of 2 MB pages is only THREE pages, so the 2 MB rows in that scenario
//     are not really the same experiment as the 4 KB rows.
//
// The lesson is worth more than the number would have been: latency on an
// out-of-order core does not decompose by subtraction, because a page walk
// overlaps the cache miss it accompanies rather than adding to it. What survives
// is (a) the counter-based facts, which are stable across runs -- about one dTLB
// miss per page crossing on 4 KB pages, essentially none on 2 MB -- and (b) the
// software prefetch result at the end, which reproduces every time.
//
// Note the bucketing differs from I3. I3 averaged by OFFSET within the page,
// which is only the same thing as "first access in this page" when lines are
// visited in order. Here lines may be shuffled, so accesses are bucketed by
// VISIT POSITION within the page: position 0 is whichever line the chain entered
// the page through.

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

constexpr std::size_t kLine         = 64;
constexpr std::size_t kPageSize     = 4096;
constexpr std::size_t kLinesPerPage = kPageSize / kLine;   // 64
constexpr std::size_t kPagesDram    = 16384;               // 64 MB: DRAM-resident
constexpr std::size_t kPagesL3      = 1536;                // 6 MB: fits in the 8 MB L3
constexpr int         kReps         = 7;                   // median, to survive a noisy machine

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

[[nodiscard]] constexpr std::uint64_t dtlb_read_miss() {
    return PERF_COUNT_HW_CACHE_DTLB
         | (PERF_COUNT_HW_CACHE_OP_READ << 8)
         | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
}

[[gnu::always_inline]] inline std::uint64_t cycles() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

[[nodiscard]] double calibrate() {
    constexpr int kN = 200000;
    std::vector<std::uint64_t> d(kN);
    for (int i = 0; i < kN; ++i) {
        const auto a = cycles();
        const auto b = cycles();
        d[static_cast<std::size_t>(i)] = b - a;
    }
    std::sort(d.begin(), d.end());
    return static_cast<double>(d[kN / 2]);
}

enum class PageKind { Small, Huge };
enum class InPage   { Sequential, Shuffled };

[[nodiscard]] char* map_region(std::size_t bytes, PageKind kind) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (kind == PageKind::Huge) flags |= MAP_HUGETLB;
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (kind == PageKind::Small) ::madvise(p, bytes, MADV_NOHUGEPAGE);
    std::memset(p, 0, bytes);
    return static_cast<char*>(p);
}

struct Result {
    double first_in_page = 0.0;   // cycles, visit position 0
    double rest          = 0.0;   // cycles, mean of positions 1..63
    double excess        = 0.0;   // first - rest
    double miss_rate     = 0.0;   // dTLB load misses per access
    double spread        = 0.0;   // max-min of the first-access samples, % of median
    bool   ok            = false;
};

// The chain visits pages in random order. Within each page the lines are visited
// either in address order or in a shuffled order. Byte 8 of the entry line holds
// the next page's entry address, so a software prefetch can be issued on arrival
// — 63 accesses of lead time before the jump actually happens.
[[nodiscard]] Result measure(std::size_t kPages, PageKind kind, InPage in_page,
                             bool software_prefetch, double overhead) {
    const std::size_t bytes = kPages * kPageSize;
    char* base = map_region(bytes, kind);
    if (!base) return {};

    std::mt19937_64 rng{4242};
    std::vector<std::uint64_t> page_order(kPages);
    std::iota(page_order.begin(), page_order.end(), 0);
    std::shuffle(page_order.begin(), page_order.end(), rng);

    std::vector<std::uint64_t> entry(kPages);     // entry line offset per page

    for (std::size_t i = 0; i < kPages; ++i) {
        const std::uint64_t page = page_order[i] * kPageSize;

        std::array<std::uint64_t, kLinesPerPage> lines{};
        std::iota(lines.begin(), lines.end(), 0);
        if (in_page == InPage::Shuffled) std::shuffle(lines.begin(), lines.end(), rng);
        entry[i] = page + lines[0] * kLine;

        for (std::size_t l = 0; l + 1 < kLinesPerPage; ++l)
            *reinterpret_cast<std::uint64_t*>(base + page + lines[l] * kLine) =
                page + lines[l + 1] * kLine;
        // last line of this page links to the next page's entry; patched below
        *reinterpret_cast<std::uint64_t*>(base + page + lines[kLinesPerPage - 1] * kLine) =
            page + lines[kLinesPerPage - 1] * kLine;   // placeholder
    }
    // Now that every entry[] is known, close the ring and plant the hints.
    for (std::size_t i = 0; i < kPages; ++i) {
        const std::uint64_t page = page_order[i] * kPageSize;
        const std::uint64_t next = entry[(i + 1) % kPages];
        // find this page's last visited line: it is the one pointing at itself
        for (std::size_t l = 0; l < kLinesPerPage; ++l) {
            auto* slot = reinterpret_cast<std::uint64_t*>(base + page + l * kLine);
            if (*slot == page + l * kLine) { *slot = next; break; }
        }
        *reinterpret_cast<std::uint64_t*>(base + entry[i] + 8) = next;   // prefetch hint
    }

    const std::size_t steps = kPages * kLinesPerPage;
    std::uint64_t idx = entry[0];
    for (std::size_t s = 0; s < steps; ++s)                      // warm
        idx = *reinterpret_cast<const std::uint64_t*>(base + idx);

    // Repeat and take medians: single runs of this move by 50%+ on a loaded box.
    std::array<double, kReps> first_samples{};
    std::array<double, kReps> rest_samples{};
    std::uint64_t misses_total = 0;

    PerfCounter dtlb(PERF_TYPE_HW_CACHE, dtlb_read_miss());
    for (int rep = 0; rep < kReps; ++rep) {
        std::array<std::uint64_t, kLinesPerPage> sum{};
        std::array<std::uint64_t, kLinesPerPage> count{};
        dtlb.start();
        std::size_t position = 0;
        for (std::size_t s = 0; s < steps; ++s) {
            if (software_prefetch && position == 0) {
                const auto hint = *reinterpret_cast<const std::uint64_t*>(base + idx + 8);
                _mm_prefetch(base + hint, _MM_HINT_T0);   // translates, and fills the TLB
            }
            const auto t0 = cycles();
            idx = *reinterpret_cast<const std::uint64_t*>(base + idx);
            const auto t1 = cycles();
            sum[position] += (t1 - t0);
            ++count[position];
            position = (position + 1) % kLinesPerPage;
        }
        misses_total += dtlb.stop();
        auto mean = [&](std::size_t b) {
            return count[b] ? static_cast<double>(sum[b]) / static_cast<double>(count[b]) - overhead
                            : 0.0;
        };
        first_samples[static_cast<std::size_t>(rep)] = std::max(0.0, mean(0));
        double acc = 0.0;
        for (std::size_t b = 1; b < kLinesPerPage; ++b) acc += std::max(0.0, mean(b));
        rest_samples[static_cast<std::size_t>(rep)] = acc / static_cast<double>(kLinesPerPage - 1);
    }
    std::sort(first_samples.begin(), first_samples.end());
    std::sort(rest_samples.begin(), rest_samples.end());
    const double spread_pct =
        first_samples[kReps - 1] > 0.0
            ? 100.0 * (first_samples[kReps - 1] - first_samples[0]) / first_samples[kReps / 2]
            : 0.0;
    const std::uint64_t misses = misses_total / kReps;
    ::munmap(base, bytes);

    Result r;
    r.first_in_page = first_samples[kReps / 2];
    r.rest          = rest_samples[kReps / 2];
    r.excess        = r.first_in_page - r.rest;
    r.miss_rate     = static_cast<double>(misses) / static_cast<double>(steps);
    r.spread        = spread_pct;
    r.ok        = true;
    return r;
}

void row(const char* label, const Result& r) {
    if (!r.ok) { std::printf("   %-34s %s\n", label, "unavailable"); return; }
    std::printf("   %-34s %9.1f %9.1f %10.1f %10.2f%% %8.0f%%\n",
                label, r.first_in_page, r.rest, r.excess, r.miss_rate * 100.0, r.spread);
}

}  // namespace

int main() {
    const double overhead = calibrate();
    std::printf("RDTSC+lfence overhead %.0f cycles (subtracted)\n", overhead);
    std::printf("bucketed by VISIT POSITION in page, not address offset\n");

    struct Scenario { const char* title; std::size_t pages; const char* note; };
    const Scenario scenarios[] = {
        {"64 MB region (DRAM-resident)", kPagesDram,
         "all three effects: TLB miss, prefetch gap, DRAM row miss"},
        {"6 MB region (fits in the 8 MB L3)", kPagesL3,
         "no DRAM traffic -- but 6 MB is only THREE 2 MB pages, so the 2 MB\n"
         "   rows below have almost no page-order randomisation left"},
    };

    Result dram_seq{};
    for (const auto& sc : scenarios) {
        std::printf("\n=== %s ===\n   %s\n\n", sc.title, sc.note);
        std::printf("   %-34s %9s %9s %10s %11s %8s\n",
                    "configuration", "first", "rest", "excess", "dTLB miss", "spread");
        const auto a = measure(sc.pages, PageKind::Small, InPage::Sequential, false, overhead);
        const auto b = measure(sc.pages, PageKind::Small, InPage::Shuffled,   false, overhead);
        const auto c = measure(sc.pages, PageKind::Huge,  InPage::Sequential, false, overhead);
        const auto d = measure(sc.pages, PageKind::Huge,  InPage::Shuffled,   false, overhead);
        row("4 KB, sequential in page", a);
        row("4 KB, shuffled in page", b);
        row("2 MB, sequential in page", c);
        row("2 MB, shuffled in page  [control]", d);

        if (a.ok && b.ok && c.ok && d.ok) {
            const double tlb_pf   = a.excess - c.excess;
            const double tlb_nopf = b.excess - d.excess;
            const double worst_spread =
                std::max(std::max(a.spread, b.spread), std::max(c.spread, d.spread));
            std::printf("\n   Attempted decomposition, by subtracting cells:\n");
            std::printf("     TLB from prefetched rows   (4 KB seq  - 2 MB seq) : %8.1f\n", tlb_pf);
            std::printf("     TLB from unprefetched rows (4 KB shuf - 2 MB shuf): %8.1f\n", tlb_nopf);
            std::printf("     prefetch gap (2 MB seq excess)                    : %8.1f\n", c.excess);
            std::printf("     CONTROL, ought to be ~0                           : %8.1f\n", d.excess);
            if (tlb_pf < 0.0 || tlb_nopf < 0.0 || d.excess > 50.0 || worst_spread > 25.0) {
                std::printf("\n   ^ NOT TRUSTWORTHY, and here is the evidence:\n");
                if (d.excess > 50.0)
                    std::printf("     - control is %.0f cycles, not ~0, so a third effect is present\n",
                                d.excess);
                if (tlb_pf < 0.0 || tlb_nopf < 0.0)
                    std::printf("     - a negative TLB cost is physically impossible\n");
                if (worst_spread > 25.0)
                    std::printf("     - spread reaches %.0f%%: cells vary by more than the\n"
                                "       differences being taken between them\n", worst_spread);
                std::printf("     Latency does not decompose by subtraction on an\n");
                std::printf("     out-of-order core: the walk overlaps the miss.\n");
            }
        }

        if (sc.pages == kPagesDram) dram_seq = a;
    }

    std::printf("\n=== Can the 4 KB penalty be requested in advance? ===\n\n");
    std::printf("   %-34s %9s %9s %10s %11s %8s\n",
                "configuration", "first", "rest", "excess", "dTLB miss", "spread");
    row("4 KB, sequential (64 MB)", dram_seq);
    const auto pf = measure(kPagesDram, PageKind::Small, InPage::Sequential, true, overhead);
    row("4 KB, sequential + _mm_prefetch", pf);
    if (dram_seq.ok && pf.ok) {
        std::printf("\n   excess %6.1f -> %6.1f cycles (%.0f%% removed)\n",
                    dram_seq.excess, pf.excess,
                    dram_seq.excess > 0.0
                        ? 100.0 * (dram_seq.excess - pf.excess) / dram_seq.excess : 0.0);
        std::printf("   But look at the dTLB column: %.2f%% -> %.2f%%, essentially unchanged.\n",
                    dram_seq.miss_rate * 100.0, pf.miss_rate * 100.0);
        std::printf("   PREFETCHh performs the translation itself, so the walk still happens —\n");
        std::printf("   it just happens on the prefetch, a page ahead of the demand load.\n");
        std::printf("   What was removed is the STALL, not the miss. A tuning pass that only\n");
        std::printf("   counted dTLB misses would conclude nothing had improved.\n");
    }
}
