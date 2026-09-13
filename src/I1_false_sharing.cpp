// I1 — false sharing: the cost of two cores writing to one cache line.
//
// Coherence is maintained per cache LINE (64 bytes here), not per variable. Two
// threads writing to different variables that happen to share a line will ping
// the line between their private caches on every store: each write invalidates
// the other core's copy, so the next write there must re-acquire the line in
// Exclusive state. The variables are independent and there is no data race —
// the program is correct, just slow. That is what makes it hard to find.
//
// Everything below writes through `volatile std::uint64_t*`, so each iteration
// really performs a load and a store rather than being optimised into a
// register. The only thing that changes between runs is the STRIDE between the
// counters: 8 bytes puts them all in one line, 64 bytes gives each its own.
//
// Thread placement matters as much as layout, so the threads are pinned. On this
// 4-core/8-thread machine the sibling pairs are (0,4) (1,5) (2,6) (3,7) — two
// siblings share an L1, so false sharing between them is far cheaper than
// between distinct cores. §6 measures that.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

namespace {

using Clock = std::chrono::steady_clock;

// std::hardware_destructive_interference_size is C++17 but libstdc++ only ships
// it from GCC 12, so fall back to the value this machine actually reports.
#if defined(__cpp_lib_hardware_interference_size)
constexpr std::size_t kLine = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kLine = 64;
#endif

constexpr std::size_t kMaxThreads = 4;
constexpr std::size_t kIters      = 20'000'000;
constexpr int         kReps       = 5;

// Room for kMaxThreads counters at up to kLine stride, page-aligned so the
// layout does not depend on where the loader happened to put it.
alignas(4096) std::array<std::uint64_t, kMaxThreads * kLine / sizeof(std::uint64_t)> g_buffer{};

[[nodiscard]] volatile std::uint64_t* counter_at(std::size_t index, std::size_t stride) {
    auto* base = reinterpret_cast<char*>(g_buffer.data());
    return reinterpret_cast<volatile std::uint64_t*>(base + index * stride);
}

void pin_to(std::thread& t, int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(t.native_handle(), sizeof(set), &set);
}

// Each thread hammers its own counter. No two threads touch the same variable,
// so there is no race — only the line they may be sharing.
void writer(volatile std::uint64_t* p, std::size_t iters) {
    for (std::size_t i = 0; i < iters; ++i) *p = *p + 1;
}

// ns per increment, median of kReps runs.
[[nodiscard]] double measure(std::size_t threads, std::size_t stride, const int* cpus) {
    std::array<double, kReps> samples{};
    for (int rep = 0; rep < kReps; ++rep) {
        std::vector<std::thread> pool;
        pool.reserve(threads);
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < threads; ++i) {
            pool.emplace_back(writer, counter_at(i, stride), kIters);
            if (cpus) pin_to(pool.back(), cpus[i]);
        }
        for (auto& t : pool) t.join();
        const auto t1 = Clock::now();
        samples[rep] = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
    }
    std::sort(samples.begin(), samples.end());
    return samples[kReps / 2];
}

// All threads READ one location; nobody writes. Sharing is free in this case —
// the line sits in Shared state in every cache at once.
void reader(const volatile std::uint64_t* p, std::size_t iters, std::uint64_t* sink) {
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < iters; ++i) acc += *p;
    *sink = acc;
}

[[nodiscard]] double measure_readonly(std::size_t threads, const int* cpus) {
    std::vector<std::uint64_t> sinks(threads);
    std::vector<std::thread> pool;
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < threads; ++i) {
        pool.emplace_back(reader, counter_at(0, 0), kIters, &sinks[i]);
        if (cpus) pin_to(pool.back(), cpus[i]);
    }
    for (auto& t : pool) t.join();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
}

// Atomic read-modify-write: already expensive on one core, catastrophic when the
// line is contended.
void atomic_writer(std::atomic<std::uint64_t>* p, std::size_t iters) {
    for (std::size_t i = 0; i < iters; ++i) p->fetch_add(1, std::memory_order_relaxed);
}

struct alignas(kLine) PaddedAtomic { std::atomic<std::uint64_t> v{0}; };

[[nodiscard]] double measure_atomic(std::size_t threads, bool padded, const int* cpus) {
    static std::array<PaddedAtomic, kMaxThreads> padded_store{};
    alignas(kLine) static std::array<std::atomic<std::uint64_t>, kMaxThreads> packed_store{};

    const auto iters = kIters / 4;          // atomics are slow; keep runtime sane
    std::vector<std::thread> pool;
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < threads; ++i) {
        auto* p = padded ? &padded_store[i].v : &packed_store[i];
        pool.emplace_back(atomic_writer, p, iters);
        if (cpus) pin_to(pool.back(), cpus[i]);
    }
    for (auto& t : pool) t.join();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

}  // namespace

int main() {
    constexpr int distinct_cores[kMaxThreads] = {0, 1, 2, 3};   // four physical cores
    constexpr int sibling_pair[2]             = {0, 4};         // one core, both SMT threads
    constexpr int distinct_pair[2]            = {0, 1};         // two cores

    std::printf("cache line: %zu bytes, %u iterations per thread, median of %d runs\n",
                kLine, static_cast<unsigned>(kIters), kReps);
#if defined(__cpp_lib_hardware_interference_size)
    std::printf("using std::hardware_destructive_interference_size\n\n");
#else
    std::printf("(std::hardware_destructive_interference_size needs GCC 12; using 64)\n\n");
#endif

    std::printf("1. Baseline: one thread, no sharing possible\n");
    std::printf("   %.2f ns per increment\n", measure(1, kLine, distinct_cores));

    std::printf("\n2. Plain writes, counters 8 bytes apart (all in ONE line)\n");
    for (std::size_t n = 2; n <= kMaxThreads; ++n)
        std::printf("   %zu threads: %7.2f ns per increment\n",
                    n, measure(n, sizeof(std::uint64_t), distinct_cores));

    std::printf("\n3. Same code, counters %zu bytes apart (one line EACH)\n", kLine);
    for (std::size_t n = 2; n <= kMaxThreads; ++n)
        std::printf("   %zu threads: %7.2f ns per increment\n", n, measure(n, kLine, distinct_cores));

    std::printf("\n4. The cost of the layout alone\n");
    for (std::size_t n = 2; n <= kMaxThreads; ++n) {
        const double shared = measure(n, sizeof(std::uint64_t), distinct_cores);
        const double padded = measure(n, kLine, distinct_cores);
        std::printf("   %zu threads: shared %7.2f vs padded %6.2f ns  -> %5.1fx slower\n",
                    n, shared, padded, shared / padded);
    }

    std::printf("\n5. Read-only sharing is free (the line is Shared everywhere)\n");
    std::printf("   4 threads reading ONE location: %.2f ns per read\n",
                measure_readonly(4, distinct_cores));
    std::printf("   contention comes from writes, not from sharing\n");

    std::printf("\n6. Placement matters: SMT siblings share an L1\n");
    std::printf("   2 threads, shared line, same core (cpu 0+4): %7.2f ns\n",
                measure(2, sizeof(std::uint64_t), sibling_pair));
    std::printf("   2 threads, shared line, two cores (cpu 0+1): %7.2f ns\n",
                measure(2, sizeof(std::uint64_t), distinct_pair));
    std::printf("   siblings never leave L1, so the line does not bounce\n");

    std::printf("\n7. Atomic fetch_add, shared line vs padded\n");
    for (std::size_t n : {2u, 4u}) {
        const double shared = measure_atomic(n, false, distinct_cores);
        const double padded = measure_atomic(n, true, distinct_cores);
        std::printf("   %zu threads: shared %7.2f vs padded %6.2f ns  -> %5.1fx slower\n",
                    n, shared, padded, shared / padded);
    }

    std::printf("\n8. Finding it in the wild\n");
    std::printf("   perf c2c record -- ./I1_false_sharing && perf c2c report\n");
    std::printf("   perf stat -e cache-misses,cache-references ./I1_false_sharing\n");
    std::printf("   the giveaway is HITM (hit-modified) on a line written by\n");
    std::printf("   more than one core; perf c2c names the offending offsets.\n");
}
