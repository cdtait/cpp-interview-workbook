// J4 — the Disruptor: a ring buffer where everyone sees everything.
//
// J2's SPSC queue moves an item from one thread to exactly one other, and the
// item is gone once popped. A Disruptor is a different shape:
//
//   - the ring is PRE-ALLOCATED and its entries are reused forever, so there is
//     no allocation on the hot path and no ownership transfer;
//   - EVERY consumer sees EVERY event. Consumers are independent readers of the
//     same ring, each tracking its own position;
//   - the producer may not overwrite an entry until the SLOWEST consumer has
//     passed it. That is "gating", and it replaces a queue's notion of "full";
//   - consumers can be chained: B may be forbidden from touching event n until
//     A has finished with it (journal, then replicate, then apply);
//   - a consumer that falls behind catches up by BATCHING. It asks "what is the
//     highest published sequence?" once and then processes everything up to it
//     with no further synchronisation.
//
// That last property is the interesting one and the reason the design exists.
// Under light load batches are size 1 and you pay one synchronisation per event.
// Under heavy load the batch grows automatically, the per-event synchronisation
// cost falls, and throughput RISES to meet the load instead of collapsing. §2
// measures exactly that.
//
// Everything here is single-producer. Multi-producer needs a separate
// availability buffer to publish out-of-order claims, which is a different
// (and much more error-prone) piece of machinery.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

namespace {

using Clock = std::chrono::steady_clock;

// A sequence counter on its own cache line. Without the padding, the producer's
// cursor and every consumer's position share lines and each publish invalidates
// them all — the I1 false-sharing problem, in the one place it hurts most.
template <bool Padded>
struct alignas(Padded ? 64 : 8) SequenceT {
    std::atomic<std::int64_t> value{-1};

    [[nodiscard]] std::int64_t load(std::memory_order o) const noexcept { return value.load(o); }
    void store(std::int64_t v, std::memory_order o) noexcept { value.store(v, o); }
};

template <typename T, std::size_t N, bool Padded = true>
class Disruptor {
    static_assert((N & (N - 1)) == 0, "ring size must be a power of two");

public:
    using Sequence = SequenceT<Padded>;

    explicit Disruptor(std::size_t consumers) : gates_(consumers) {}

    // --- producer -----------------------------------------------------------
    // Claim the next slot, waiting until the slowest consumer has passed the
    // entry we are about to overwrite. This is back-pressure without a "full"
    // flag: the ring is full exactly when someone still needs the oldest entry.
    [[nodiscard]] std::int64_t claim() noexcept {
        const std::int64_t next = cursor_.load(std::memory_order_relaxed) + 1;
        const std::int64_t wrap = next - static_cast<std::int64_t>(N);
        if (wrap > cached_gate_) {                 // cheap check against a stale value
            while (wrap > (cached_gate_ = slowest_consumer())) {
                ++stalls_;
                _mm_pause();
            }
        }
        return next;
    }

    [[nodiscard]] T& operator[](std::int64_t sequence) noexcept {
        return slots_[static_cast<std::size_t>(sequence) & (N - 1)];
    }

    // The release store is the publish: everything written to the slot above
    // must be visible to any consumer that then observes this cursor value.
    void publish(std::int64_t sequence) noexcept {
        cursor_.store(sequence, std::memory_order_release);
    }

    // --- consumer -----------------------------------------------------------
    [[nodiscard]] Sequence& gate(std::size_t index) noexcept { return gates_[index]; }

    // Wait until `sequence` is available, then report the HIGHEST sequence
    // available right now. The gap between the two is the batch.
    [[nodiscard]] std::int64_t wait_for(std::int64_t sequence,
                                        const Sequence* depends_on,
                                        const std::atomic<bool>& stop) const noexcept {
        for (;;) {
            const std::int64_t available =
                depends_on ? depends_on->load(std::memory_order_acquire)
                           : cursor_.load(std::memory_order_acquire);
            if (available >= sequence) return available;
            if (stop.load(std::memory_order_relaxed)) return available;
            _mm_pause();
        }
    }

    [[nodiscard]] std::int64_t cursor() const noexcept {
        return cursor_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

private:
    [[nodiscard]] std::int64_t slowest_consumer() const noexcept {
        std::int64_t lowest = cursor_.load(std::memory_order_relaxed);
        for (const auto& g : gates_)
            lowest = std::min(lowest, g.load(std::memory_order_acquire));
        return lowest;
    }

    std::array<T, N> slots_{};
    Sequence         cursor_;                 // written only by the producer
    std::vector<Sequence> gates_;             // one per consumer
    std::int64_t     cached_gate_ = -1;       // producer-private
    std::uint64_t    stalls_ = 0;
};

struct Event {
    std::int64_t  sequence;
    std::uint64_t payload;
};

[[nodiscard]] constexpr std::uint64_t mix(std::int64_t v) {
    auto x = static_cast<std::uint64_t>(v);
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return x;
}

void pin_self(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

struct ConsumerStats {
    std::uint64_t events = 0;
    std::uint64_t batches = 0;
    std::int64_t  max_batch = 0;
    bool          ordered = true;     // saw every sequence, exactly once, in order
};

constexpr std::size_t kRing = 4096;
using Ring = Disruptor<Event, kRing>;

// One consumer: wait, then drain everything available in a single batch.
ConsumerStats run_consumer(Ring& ring, std::size_t gate_index,
                           const Ring::Sequence* depends_on,
                           std::int64_t total, const std::atomic<bool>& stop,
                           std::uint32_t work_per_event) {
    ConsumerStats st;
    auto& mine = ring.gate(gate_index);
    std::int64_t next = 0;
    std::uint64_t sink = 0;

    while (next < total) {
        const std::int64_t available = ring.wait_for(next, depends_on, stop);
        if (available < next) break;                       // stopped
        const std::int64_t batch = available - next + 1;
        st.max_batch = std::max(st.max_batch, batch);
        ++st.batches;

        for (std::int64_t s = next; s <= available; ++s) {
            const Event& e = ring[s];
            if (e.sequence != s || e.payload != mix(s)) st.ordered = false;
            for (std::uint32_t w = 0; w < work_per_event; ++w) sink = mix(static_cast<std::int64_t>(sink ^ e.payload));
            ++st.events;
        }
        next = available + 1;
        // Publishing our position is what releases the producer's gate.
        mine.store(available, std::memory_order_release);
    }
    if (sink == 0x123456789ULL) std::printf(" ");      // keep `sink` alive
    return st;
}

struct RunResult {
    double ns_per_event;
    std::vector<ConsumerStats> consumers;
    std::uint64_t producer_stalls;
};

[[nodiscard]] RunResult run(std::size_t consumer_count, std::int64_t total,
                            std::uint32_t work_per_event, bool chained) {
    Ring ring(consumer_count);
    std::atomic<bool> stop{false};
    std::vector<ConsumerStats> stats(consumer_count);
    std::vector<std::thread> threads;

    for (std::size_t i = 0; i < consumer_count; ++i) {
        // In a chain, consumer i waits on consumer i-1 instead of the cursor.
        const Ring::Sequence* dep =
            (chained && i > 0) ? &ring.gate(i - 1) : nullptr;
        threads.emplace_back([&, i, dep] {
            pin_self(static_cast<int>(i + 1));
            stats[i] = run_consumer(ring, i, dep, total, stop, work_per_event);
        });
    }

    const auto t0 = Clock::now();
    std::thread producer([&] {
        pin_self(0);
        for (std::int64_t s = 0; s < total; ++s) {
            const std::int64_t seq = ring.claim();
            ring[seq] = Event{seq, mix(seq)};
            ring.publish(seq);
        }
    });
    producer.join();
    for (auto& t : threads) t.join();
    const auto t1 = Clock::now();
    stop.store(true, std::memory_order_relaxed);

    return { std::chrono::duration<double, std::nano>(t1 - t0).count()
                 / static_cast<double>(total),
             std::move(stats), ring.stalls() };
}

}  // namespace

int main() {
    constexpr std::int64_t kEvents = 5'000'000;

    std::printf("Disruptor: ring of %zu entries, single producer, pinned threads\n\n", kRing);

    std::printf("1. Fan-out: every consumer sees every event\n");
    std::printf("   %-12s %14s %14s %12s %10s\n",
                "consumers", "ns per event", "events each", "all correct", "stalls");
    for (std::size_t n : {1u, 2u, 3u}) {
        const auto r = run(n, kEvents, 0, false);
        bool all_ok = true;
        std::uint64_t each = 0;
        for (const auto& c : r.consumers) {
            all_ok = all_ok && c.ordered && c.events == static_cast<std::uint64_t>(kEvents);
            each = c.events;
        }
        std::printf("   %-12zu %14.2f %14llu %12s %10llu\n", n, r.ns_per_event,
                    static_cast<unsigned long long>(each), all_ok ? "yes" : "NO",
                    static_cast<unsigned long long>(r.producer_stalls));
    }
    std::printf("\n   'all correct' means each consumer saw every sequence exactly once, in\n");
    std::printf("   order, with an intact payload. Nothing was copied to achieve that: they\n");
    std::printf("   all read the same ring entries.\n");

    std::printf("\n2. Batching: the property the design exists for\n");
    std::printf("   Same ring, one consumer, given increasing work per event.\n");
    std::printf("   %-14s %14s %12s %12s %12s\n",
                "work/event", "ns per event", "batches", "mean batch", "max batch");
    for (std::uint32_t work : {0u, 4u, 16u, 64u}) {
        const auto r = run(1, kEvents / 5, work, false);
        const auto& c = r.consumers[0];
        const double mean = c.batches ? static_cast<double>(c.events) / static_cast<double>(c.batches) : 0.0;
        std::printf("   %-14u %14.2f %12llu %12.1f %12lld\n", work, r.ns_per_event,
                    static_cast<unsigned long long>(c.batches), mean,
                    static_cast<long long>(c.max_batch));
    }
    std::printf("\n   As the consumer slows down it falls behind, so each wait returns more\n");
    std::printf("   events and the batch grows. One synchronisation now covers many events,\n");
    std::printf("   which is why a Disruptor degrades gently under load instead of\n");
    std::printf("   thrashing: the busier it gets, the less it synchronises per event.\n");

    std::printf("\n3. Gating: the producer cannot lap the slowest consumer\n");
    {
        const auto fast = run(1, kEvents / 10, 0, false);
        const auto slow = run(1, kEvents / 10, 64, false);
        std::printf("   fast consumer: %llu producer stalls\n",
                    static_cast<unsigned long long>(fast.producer_stalls));
        std::printf("   slow consumer: %llu producer stalls\n",
                    static_cast<unsigned long long>(slow.producer_stalls));
        std::printf("   A stall is the producer spinning because overwriting the next entry\n");
        std::printf("   would destroy something a consumer has not read. There is no 'full'\n");
        std::printf("   flag — fullness IS the slowest consumer's position, %zu behind.\n", kRing);
    }

    std::printf("\n4. Dependency chains: consumer B strictly after consumer A\n");
    {
        const auto r = run(3, kEvents / 10, 0, true);
        bool ok = true;
        for (const auto& c : r.consumers) ok = ok && c.ordered;
        std::printf("   3 consumers chained A -> B -> C: all correct = %s, %.2f ns/event\n",
                    ok ? "yes" : "NO", r.ns_per_event);
        std::printf("   Each waits on the PREVIOUS consumer's sequence rather than the\n");
        std::printf("   cursor, so C cannot touch event n until B has, and B not until A has.\n");
        std::printf("   That is journal -> replicate -> apply, with no queues between stages.\n");
    }

    std::printf("\n5. Why it is not just a queue\n");
    std::printf("   - entries are reused in place: no allocation, no ownership transfer,\n");
    std::printf("     and the ring stays resident in cache (see I1 and I3)\n");
    std::printf("   - fan-out is free: N consumers read the same entry, they do not each\n");
    std::printf("     get a copy, which is where FeedB and FeedC in F1 spend their time\n");
    std::printf("   - back-pressure is inherent: the producer is gated on real progress,\n");
    std::printf("     not on a counter that says 'full'\n");
    std::printf("   - batching is automatic and self-tuning: it costs nothing when idle\n");
    std::printf("     and saves the most exactly when you are busiest\n");
    std::printf("   - one writer per sequence, so every store is a plain release store —\n");
    std::printf("     no locked instruction anywhere on the hot path (J1 section 1)\n");
}
