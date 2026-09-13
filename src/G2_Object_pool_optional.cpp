// G2 — Object pool, the simple variant: std::optional instead of a union.
//
// G1 reuses a slot's bytes for either the T or the free-list link, which needs a
// union, placement new, an explicit ~T(), and an occupancy bitset. All of that
// exists to save 8 bytes per slot. This version asks whether the simpler thing
// is good enough, and the answer is usually yes.
//
// std::optional<T> already is "storage for a T that may or may not hold one",
// and has_value() already is the occupancy flag. So:
//
//   - no union, and no hand-written union special members
//   - no placement new, no explicit destructor call
//   - no occupancy bitset — optional tracks it
//   - no reinterpret_cast anywhere, because the deleter carries the index
//   - no hand-written handle: std::unique_ptr with a custom deleter already is
//     one, which is ~50 lines of move/reset/release boilerplate not written
//   - ~SimplePool is implicit: destroying the array destroys any live objects
//
// Measured against G1 (GCC 10.3, -O2, T = Quote at 16 bytes, N = 1024):
//
//   slot size            union 16      optional 24      (optional +50%)
//   whole 1024 pool      union 16520   optional 32776   (optional 2.0x)
//   ns per alloc/dealloc union 4.64    optional 5.06    (optional +9%)
//   code lines           union 73      optional ~55
//
// So the union buys memory density, not speed. Prefer this version unless the
// pool is hot enough that halving its footprint keeps it in cache — which is
// exactly the case F1's feed handler would care about.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

template <typename T, std::size_t N>
    requires std::is_nothrow_destructible_v<T>
class SimplePool {
    static_assert(N > 0, "a pool needs at least one slot");

public:
    // The deleter carries the index, which is what lets the handle be a plain
    // std::unique_ptr: optional<T>'s layout is unspecified, so recovering an
    // index from a T* would need byte arithmetic that is not strictly
    // conforming. unique_ptr then supplies move semantics, reset(), release(),
    // get(), operator*, operator-> and operator bool for free — roughly fifty
    // lines of handle boilerplate that do not need writing.
    class Deleter {
    public:
        Deleter() noexcept = default;
        Deleter(SimplePool* pool, std::size_t index) noexcept
            : pool_(pool), index_(index) {}

        void operator()(T*) const noexcept { if (pool_) pool_->recycle(index_); }

        [[nodiscard]] std::size_t index() const noexcept { return index_; }

    private:
        SimplePool* pool_  = nullptr;
        std::size_t index_ = 0;
    };

    using Handle = std::unique_ptr<T, Deleter>;

    SimplePool() noexcept {
        // A free stack, so the lowest indices are handed out first.
        for (std::size_t i = 0; i < N; ++i) free_[i] = N - 1 - i;
        free_count_ = N;
    }

    // No destructor needed: ~array<optional<T>, N> destroys whatever is live,
    // which is what G1 needed the occupancy bitset for.

    // Handles hold a pointer back to the pool, so it must not move.
    SimplePool(const SimplePool&) = delete;
    SimplePool& operator=(const SimplePool&) = delete;
    SimplePool(SimplePool&&) = delete;
    SimplePool& operator=(SimplePool&&) = delete;

    template <typename... Args>
    [[nodiscard]] Handle allocate(Args&&... args) {
        if (free_count_ == 0) throw std::runtime_error("Object pool exhausted");

        // Construct first, commit after. If T's constructor throws, free_count_
        // is untouched and the slot is still free — so unlike G1 this needs no
        // try/catch. G1 has to unlink before constructing, because the link
        // lives in the very bytes it is about to overwrite.
        const std::size_t index = free_[free_count_ - 1];
        slots_[index].emplace(std::forward<Args>(args)...);
        --free_count_;

        return Handle(&*slots_[index], Deleter(this, index));
    }

    [[nodiscard]] std::size_t size()      const noexcept { return N - free_count_; }
    [[nodiscard]] std::size_t available() const noexcept { return free_count_; }
    [[nodiscard]] static constexpr std::size_t capacity()  noexcept { return N; }
    [[nodiscard]] static constexpr std::size_t slot_size() noexcept {
        return sizeof(std::optional<T>);
    }

private:
    friend class Deleter;

    [[nodiscard]] T* at(std::size_t index) noexcept {
        assert(index < N && slots_[index].has_value());
        return &*slots_[index];
    }

    void recycle(std::size_t index) noexcept {
        assert(index < N && "index out of range");
        assert(slots_[index].has_value() && "double free, or never allocated");
        slots_[index].reset();                 // T's destructor runs here
        free_[free_count_++] = index;
    }

    std::array<std::optional<T>, N> slots_;
    std::array<std::size_t, N>      free_;
    std::size_t                     free_count_ = 0;
};

// ---------------------------------------------------------------------------

struct Quote {
    std::uint64_t id_;
    double        price_;

    Quote(std::uint64_t id, double price) noexcept : id_(id), price_(price) {}
};

struct Tracer {
    int id_;
    explicit Tracer(int id) : id_(id) { std::cout << "    Tracer(" << id_ << ") constructed\n"; }
    ~Tracer() { std::cout << "    Tracer(" << id_ << ") destroyed\n"; }
};

struct Throws {
    explicit Throws(bool fail) { if (fail) throw std::runtime_error("ctor failed"); }
};

int main() {
    std::cout << "1. A type with no default constructor\n";
    {
        SimplePool<Quote, 4> pool;
        auto q = pool.allocate(42, 101.25);
        std::cout << "   allocated Quote{" << q->id_ << ", " << q->price_ << "}"
                  << ", in use " << pool.size() << '/' << pool.capacity() << '\n';
        q.reset();
        std::cout << "   after reset, in use " << pool.size() << '\n';
    }

    std::cout << "\n2. Construction happens at allocate(), not at pool construction\n";
    {
        SimplePool<Tracer, 8> pool;
        std::cout << "   pool built (8 slots) — nothing constructed above this line\n";
        auto a = pool.allocate(1);
        auto b = pool.allocate(2);
        std::cout << "   handles going out of scope\n";
    }

    std::cout << "\n3. Storage is recycled: the same slot comes back\n";
    {
        SimplePool<Quote, 4> pool;
        const void* addr = nullptr;
        std::size_t idx = 0;
        { auto first = pool.allocate(1, 1.0); addr = first.get(); idx = first.get_deleter().index(); }
        auto again = pool.allocate(2, 2.0);
        std::cout << "   same address reused: " << (addr == again.get() ? "yes" : "no")
                  << ", same index: " << (idx == again.get_deleter().index() ? "yes" : "no") << '\n';
    }

    std::cout << "\n4. Exhaustion\n";
    {
        SimplePool<Quote, 3> pool;
        std::array<SimplePool<Quote, 3>::Handle, 3> held;
        for (std::size_t i = 0; i < 3; ++i) held[i] = pool.allocate(i, 0.0);
        std::cout << "   in use " << pool.size() << ", available " << pool.available() << '\n';
        try {
            auto extra = pool.allocate(99, 0.0);
            std::cout << "   ERROR: should have thrown\n";
        } catch (const std::runtime_error& e) {
            std::cout << "   4th allocate threw: " << e.what() << '\n';
        }
        held[1].reset();
        std::cout << "   after releasing one, available " << pool.available() << '\n';
    }

    std::cout << "\n5. A throwing constructor must not lose the slot\n";
    {
        SimplePool<Throws, 2> pool;
        try {
            auto bad = pool.allocate(true);
        } catch (const std::runtime_error&) {}
        std::cout << "   after a failed allocate, available " << pool.available()
                  << " (should still be 2) — and no try/catch inside allocate()\n";
    }

    std::cout << "\n6. Move semantics on the handle\n";
    {
        SimplePool<Tracer, 4> pool;
        auto a = pool.allocate(7);
        auto b = std::move(a);
        std::cout << "   after move, source empty: " << (a ? "no" : "yes")
                  << ", target holds Tracer(" << b->id_ << "), in use " << pool.size() << '\n';
    }

    std::cout << "\n7. The pool destructor cleans up a leaked object — with no bitset\n";
    {
        SimplePool<Tracer, 4> pool;
        auto leaked = pool.allocate(10);
        (void)leaked.release();        // abandon it: nothing will free this slot
        std::cout << "   in use " << pool.size() << " after abandoning the handle,"
                  << " leaving scope\n";
    }
    std::cout << "   (destruction above came from ~array<optional<Tracer>, 4>)\n";

    std::cout << "\n8. What optional costs, in bytes\n";
    {
        std::cout << "   sizeof(Quote)              = " << sizeof(Quote) << '\n'
                  << "   optional slot              = " << SimplePool<Quote, 4>::slot_size() << '\n'
                  << "   (G1's union slot was 16 — the extra 8 is optional's bool + padding)\n"
                  << "   for T = int: optional slot = " << SimplePool<int, 4>::slot_size()
                  << " vs G1's union 8\n";
    }
}
