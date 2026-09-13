#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>


std::atomic<std::size_t> g_allocs{0};
void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using Clock = std::chrono::steady_clock;

// std::atomic<std::shared_ptr<T>> is C++20 (P0718), but libstdc++ only ships it
// from GCC 12. On older toolchains fall back to the free functions, which give
// the same guarantees; they are deprecated in C++20, hence the local pragma.
#if defined(__cpp_lib_atomic_shared_ptr)
template <typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;
#else
template <typename T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() = default;
    AtomicSharedPtr(std::shared_ptr<T> p) : p_(std::move(p)) {}

    std::shared_ptr<T> load(std::memory_order o = std::memory_order_seq_cst) const {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        return std::atomic_load_explicit(&p_, o);
#pragma GCC diagnostic pop
    }
    void store(std::shared_ptr<T> v, std::memory_order o = std::memory_order_seq_cst) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        std::atomic_store_explicit(&p_, std::move(v), o);
#pragma GCC diagnostic pop
    }
private:
    std::shared_ptr<T> p_;
};
#endif

struct Level { std::int64_t px; std::uint32_t qty; };

struct Book {
    std::vector<Level> bids;
    std::uint64_t      seq = 0;
    explicit Book(std::size_t depth = 0) : bids(depth, Level{0, 0}) {}
};

struct Packet { std::uint32_t level; std::int64_t px; std::uint32_t qty; };

using Callback = std::function<void(const Book&)>;

// A trivial subscriber, so the measurement is of the plumbing.
std::int64_t g_sink = 0;
void cheap_subscriber(const Book& b) { g_sink += b.bids[0].px + static_cast<std::int64_t>(b.seq); }

// ===========================================================================
// A — the original: callbacks invoked while the lock is held.
// ===========================================================================
class FeedA {
public:
    void subscribe(Callback f) {
        std::lock_guard<std::mutex> lk(m_);
        callbacks_.push_back(std::move(f));
    }
    void on_packet(const Packet& p) {
        std::lock_guard<std::mutex> lk(m_);
        apply(p);
        for (auto& cb : callbacks_) cb(book_);        // lock held across user code
    }
    Book& book() { return book_; }
    void resize(std::size_t d) { book_ = Book(d); }
    std::int64_t best_bid() { std::lock_guard<std::mutex> lk(m_); return book_.bids[0].px; }
private:
    void apply(const Packet& p) {
        book_.bids[p.level] = Level{p.px, p.qty};
        ++book_.seq;
    }
    std::mutex m_;
    Book book_{1};
    std::vector<Callback> callbacks_;
};

// ===========================================================================
// B — copy the book and the subscriber list under the lock, notify outside.
//     Safe against any callback. Costs a full book copy per packet.
// ===========================================================================
class FeedB {
public:
    void subscribe(Callback f) {
        std::lock_guard<std::mutex> lk(m_);
        callbacks_.push_back(std::move(f));
    }
    void on_packet(const Packet& p) {
        Book snapshot(0);
        std::vector<Callback> subs;
        {
            std::lock_guard<std::mutex> lk(m_);
            book_.bids[p.level] = Level{p.px, p.qty};
            ++book_.seq;
            snapshot = book_;                          // deep copy
            subs = callbacks_;                         // list copy
        }
        for (auto& cb : subs) cb(snapshot);            // no lock held
    }
    void resize(std::size_t d) { book_ = Book(d); }
private:
    std::mutex m_;
    Book book_{1};
    std::vector<Callback> callbacks_;
};

// ===========================================================================
// C — copy-on-write. One Book allocation per packet regardless of subscriber
//     count; subscribers hold an immutable version for as long as they like.
// ===========================================================================
class FeedC {
public:
    void subscribe(Callback f) {
        std::lock_guard<std::mutex> lk(m_);
        auto current = callbacks_.load(std::memory_order_acquire);
        auto next =
            std::make_shared<std::vector<Callback>>(*current);
        next->push_back(std::move(f));
        callbacks_.store(
            std::shared_ptr<const std::vector<Callback>>(std::move(next)),
            std::memory_order_release
        );
    }

    void on_packet(const Packet& p) {
        std::shared_ptr<const Book> published;
        {
            std::lock_guard<std::mutex> lk(m_);
            auto next = std::make_shared<Book>(*book_);
            next->bids[p.level] = Level{p.px, p.qty};
            ++next->seq;
            book_ = next;
            published = std::move(next);
        }

        auto subs =
            callbacks_.load(std::memory_order_acquire);

        for (const auto& cb : *subs)
            cb(*published);
    }

    void resize(std::size_t d) {
        std::lock_guard<std::mutex> lk(m_);
        book_ = std::make_shared<Book>(d);
    }

private:
    std::mutex m_;

    std::shared_ptr<const Book> book_ =
        std::make_shared<Book>(1);
    AtomicSharedPtr<const std::vector<Callback>> callbacks_{
        std::make_shared<const std::vector<Callback>>()
    };
};

// ===========================================================================
// D — single writer thread owns the book. No mutex anywhere on the hot path.
//     Callbacks get a direct const& and must not re-enter or block.
//     A seqlock lets outside readers take a consistent snapshot.
// ===========================================================================
class FeedD {
public:
    void subscribe(Callback f) { callbacks_.push_back(std::move(f)); }  // setup only

    void on_packet(const Packet& p) {
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        book_.bids[p.level] = Level{p.px, p.qty};
        ++book_.seq;
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);

        for (auto& cb : callbacks_) cb(book_);        // no copy, no lock
    }

    // Called from any other thread. Retries while the writer is mid-update.
    bool read_snapshot(Book& out) const {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1) continue;                     // writer in progress
            out = book_;
            const auto s2 = seq_.load(std::memory_order_acquire);
            if (s1 == s2) return true;                // no update in between
        }
        return false;
    }

    void resize(std::size_t d) { book_ = Book(d); }
private:
    alignas(64) std::atomic<std::uint64_t> seq_{0};
    Book book_{1};
    std::vector<Callback> callbacks_;
};

// ---------------------------------------------------------------------------

constexpr std::size_t kPackets = 100000;

Packet make_packet(std::size_t i, std::size_t depth) {
    return Packet{static_cast<std::uint32_t>(i % depth),
                  100'000 + static_cast<std::int64_t>(i % 500),
                  static_cast<std::uint32_t>(500 + i % 7)};
}

template <typename Feed>
void bench(const char* name, std::size_t depth, std::size_t subscribers, double* out_ns,
           std::size_t* out_allocs) {
    Feed feed;
    feed.resize(depth);
    for (std::size_t i = 0; i < subscribers; ++i) feed.subscribe(cheap_subscriber);

    // Warm up so allocator growth is not counted.
    for (std::size_t i = 0; i < 1000; ++i) feed.on_packet(make_packet(i, depth));

    const auto a0 = g_allocs.load();
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < kPackets; ++i) feed.on_packet(make_packet(i, depth));
    const auto t1 = Clock::now();

    *out_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kPackets;
    *out_allocs = g_allocs.load() - a0;
    (void)name;
}

// ---------------------------------------------------------------------------
// Safety: what happens when a subscriber misbehaves.
// ---------------------------------------------------------------------------
bool survives_reentrant_call() {
    // Static so the deliberately-deadlocked thread never touches a dead object.
    // A std::async future would be wrong here: its destructor blocks until the
    // task finishes, and this task never does.
    static FeedA feed;
    static std::atomic<bool> done{false};
    feed.resize(4);
    feed.subscribe([](const Book&) { (void)feed.best_bid(); });   // re-enters the mutex
    std::thread([] { feed.on_packet(Packet{0, 1, 1}); done.store(true); }).detach();
    for (int i = 0; i < 100 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return done.load();
}

template <typename Feed>
bool survives_subscribing_callback() {
    Feed feed;
    feed.resize(4);
    std::atomic<bool> added{false};
    feed.subscribe([&](const Book&) {
        if (!added.exchange(true)) feed.subscribe(cheap_subscriber);  // mutates the list
    });
    feed.on_packet(Packet{0, 1, 1});
    feed.on_packet(Packet{1, 2, 2});
    return true;                       // reaching here without crashing
}

}  // namespace

int main() {
    std::printf("packets: %zu, subscribers: 4\n\n", kPackets);
    std::printf("cost per packet (ns), and heap allocations per packet\n");
    std::printf("%10s %22s %22s %22s %22s\n", "depth",
                "A under-lock", "B snapshot", "C copy-on-write", "D single-writer");

    for (std::size_t depth : {16u, 128u, 1024u}) {
        double na, nb, nc, nd;
        std::size_t aa, ab, ac, ad;
        bench<FeedA>("A", depth, 4, &na, &aa);
        bench<FeedB>("B", depth, 4, &nb, &ab);
        bench<FeedC>("C", depth, 4, &nc, &ac);
        bench<FeedD>("D", depth, 4, &nd, &ad);
        std::printf("%10zu %14.1f %7.2f %14.1f %7.2f %14.1f %7.2f %14.1f %7.2f\n",
                    depth,
                    na, double(aa) / kPackets, nb, double(ab) / kPackets,
                    nc, double(ac) / kPackets, nd, double(ad) / kPackets);
    }

    std::printf("\nsafety\n");
    std::printf("  A survives a callback that calls back in : %s\n",
                survives_reentrant_call() ? "yes" : "NO (deadlock)");
    std::printf("  B survives a callback that subscribes    : %s\n",
                survives_subscribing_callback<FeedB>() ? "yes" : "no");
    std::printf("  C survives a callback that subscribes    : %s\n",
                survives_subscribing_callback<FeedC>() ? "yes" : "no");

    // D's seqlock: an outside reader takes consistent snapshots while the
    // writer runs flat out.
    {
        FeedD feed;
        feed.resize(128);
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> ok{0}, retry{0};
        std::thread reader([&] {
            Book snap(0);
            while (!stop.load(std::memory_order_relaxed)) {
                if (feed.read_snapshot(snap)) ++ok; else ++retry;
            }
        });
        for (std::size_t i = 0; i < 200000; ++i) {
            feed.on_packet(make_packet(i, 128));
            // On a single core the reader would otherwise never be scheduled
            // before this loop finishes.
            if ((i & 0x1FF) == 0) std::this_thread::yield();
        }
        stop.store(true);
        reader.join();
        std::printf("  D seqlock reader: %zu consistent snapshots, %zu gave up\n",
                    ok.load(), retry.load());
    }
    return 0;
}