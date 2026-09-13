// G1 — Object pool: decoupling storage from object lifetime.
//
// The naive pool stores `T object;` in every slot, which forces all N objects
// to be default-constructed when the pool is constructed. That is an array with
// a reuse list, not a pool: it rules out every T without a default constructor,
// and it pays for N objects you have not asked for yet.
//
// A pool's defining property is that *storage* is reserved up front but object
// *lifetime* begins at allocate() and ends at deallocate(). Getting there needs
// three pieces:
//
//   1. A union slot, so raw storage for a T and the free-list link share the
//      same bytes — they are never needed at the same time.
//   2. An intrusive free list: each free slot holds the index of the next free
//      slot, so allocate/deallocate are O(1) with no container of their own.
//   3. Explicit lifetime management: placement new in allocate, ~T() in
//      deallocate, and an occupancy bitset so the destructor can clean up after
//      a caller who leaked a handle.

#include <array>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

// is_nothrow_destructible subsumes is_destructible, and default-constructible
// is exactly the requirement we set out to remove, so one constraint is left.
// It earns its place: it is what lets deallocate() and ~ObjectPool() be noexcept.
template <typename T, std::size_t N>
    requires std::is_nothrow_destructible_v<T>
class ObjectPool {
    static_assert(N > 0, "a pool needs at least one slot");

    // FREE:      next_free is the active member
    // ALLOCATED: object is the active member
    // Because T is non-trivial in general, the union's special members are
    // deleted unless written by hand. ~Slot() is empty on purpose: the pool
    // destroys live objects itself, via the occupancy bitset.
    union Slot {
        std::size_t next_free;
        T           object;

        Slot() noexcept : next_free(0) {}
        ~Slot() {}

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };

    static constexpr std::size_t kEnd = N;   // end-of-list sentinel

public:
    ObjectPool() noexcept {
        // Thread every slot onto the free list: 0 -> 1 -> 2 -> ... -> kEnd.
        for (std::size_t i = 0; i < N; ++i) pool_[i].next_free = i + 1;
        free_head_ = 0;
    }

    ~ObjectPool() {
        // A caller may have dropped a raw pointer without deallocating. The
        // bitset is the only way to tell an occupied slot from a free one.
        for (std::size_t i = 0; i < N; ++i)
            if (occupied_[i]) pool_[i].object.~T();
    }

    // Slot addresses are handed out to callers, so the pool cannot be copied or
    // moved without dangling them.
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    // T is constructed here and nowhere else. Any constructor signature works,
    // so a type with no default constructor is no longer a problem.
    template <typename... Args>
    [[nodiscard]] T* allocate(Args&&... args) {
        if (free_head_ == kEnd) throw std::runtime_error("Object pool exhausted");

        const std::size_t index = free_head_;
        free_head_ = pool_[index].next_free;      // unlink before constructing

        T* result;
        try {
            result = new (&pool_[index].object) T(std::forward<Args>(args)...);
        } catch (...) {
            pool_[index].next_free = free_head_;  // relink, or the slot is lost
            free_head_ = index;
            throw;
        }

        occupied_.set(index);
        return result;
    }

    void deallocate(T* obj) noexcept {
        if (obj == nullptr) return;

        // A union is pointer-interconvertible with each of its non-static data
        // members, so this cast is well-defined — and it is what makes the
        // reverse index lookup legal. (T* - Slot* would not even compile.)
        Slot* slot = reinterpret_cast<Slot*>(obj);
        const std::size_t index = static_cast<std::size_t>(slot - pool_.data());

        assert(index < N && "pointer does not belong to this pool");
        assert(occupied_[index] && "double free, or never allocated");

        obj->~T();
        occupied_.reset(index);

        slot->next_free = free_head_;             // push onto the front
        free_head_ = index;
    }

    // An owning handle, so the common case cannot leak. Movable, not copyable.
    class Deleter {
    public:
        Deleter() noexcept = default;
        explicit Deleter(ObjectPool* pool) noexcept : pool_(pool) {}
        void operator()(T* obj) const noexcept { if (pool_) pool_->deallocate(obj); }
    private:
        ObjectPool* pool_ = nullptr;
    };
    using Handle = std::unique_ptr<T, Deleter>;

    template <typename... Args>
    [[nodiscard]] Handle make(Args&&... args) {
        return Handle(allocate(std::forward<Args>(args)...), Deleter(this));
    }

    [[nodiscard]] std::size_t size()      const noexcept { return occupied_.count(); }
    [[nodiscard]] std::size_t available() const noexcept { return N - occupied_.count(); }
    [[nodiscard]] static constexpr std::size_t capacity()  noexcept { return N; }
    [[nodiscard]] static constexpr std::size_t slot_size() noexcept { return sizeof(Slot); }

private:
    std::array<Slot, N> pool_;
    std::bitset<N>      occupied_;
    std::size_t         free_head_ = kEnd;
};

// ---------------------------------------------------------------------------

struct Quote {
    std::uint64_t id_;
    double        price_;

    Quote(std::uint64_t id, double price) noexcept : id_(id), price_(price) {}
};

// Announces its own lifetime, to show when construction actually happens.
struct Tracer {
    int id_;
    explicit Tracer(int id) : id_(id) {
        std::cout << "    Tracer(" << id_ << ") constructed\n";
    }
    ~Tracer() { std::cout << "    Tracer(" << id_ << ") destroyed\n"; }
};

struct Throws {
    Throws(bool fail) { if (fail) throw std::runtime_error("ctor failed"); }
};

int main() {
    std::cout << "1. A type with no default constructor\n";
    {
        ObjectPool<Quote, 4> pool;
        Quote* q = pool.allocate(42, 101.25);        // forwarded to Quote(id, price)
        std::cout << "   allocated Quote{" << q->id_ << ", " << q->price_ << "}"
                  << ", in use " << pool.size() << '/' << pool.capacity() << '\n';
        pool.deallocate(q);
        std::cout << "   after deallocate, in use " << pool.size() << '\n';
    }

    std::cout << "\n2. Construction happens at allocate(), not at pool construction\n";
    {
        ObjectPool<Tracer, 8> pool;
        std::cout << "   pool built (8 slots) — nothing constructed above this line\n";
        Tracer* a = pool.allocate(1);
        Tracer* b = pool.allocate(2);
        pool.deallocate(a);
        pool.deallocate(b);
        std::cout << "   pool going out of scope\n";
    }

    std::cout << "\n3. Storage is recycled: the same slot comes back\n";
    {
        ObjectPool<Quote, 4> pool;
        Quote* first = pool.allocate(1, 1.0);
        const void* addr = first;
        pool.deallocate(first);
        Quote* again = pool.allocate(2, 2.0);
        std::cout << "   same address reused: "
                  << (addr == again ? "yes" : "no") << '\n';
        pool.deallocate(again);
    }

    std::cout << "\n4. Exhaustion, and the free list surviving it\n";
    {
        ObjectPool<Quote, 3> pool;
        Quote* held[3];
        for (int i = 0; i < 3; ++i) held[i] = pool.allocate(std::uint64_t(i), 0.0);
        std::cout << "   in use " << pool.size() << ", available " << pool.available() << '\n';
        try {
            (void)pool.allocate(99, 0.0);
            std::cout << "   ERROR: should have thrown\n";
        } catch (const std::runtime_error& e) {
            std::cout << "   4th allocate threw: " << e.what() << '\n';
        }
        pool.deallocate(held[1]);
        std::cout << "   after freeing one, available " << pool.available()
                  << "; re-allocating works: " << (pool.allocate(7, 7.0) != nullptr) << '\n';
        // held[0], held[2] and the last one are deliberately leaked —
        // ~ObjectPool cleans them up via the occupancy bitset.
        std::cout << "   leaving scope with " << pool.size() << " objects still live\n";
    }

    std::cout << "\n5. A throwing constructor must not lose the slot\n";
    {
        ObjectPool<Throws, 2> pool;
        try { (void)pool.allocate(true); } catch (const std::runtime_error&) {}
        std::cout << "   after a failed allocate, available " << pool.available()
                  << " (should still be 2)\n";
    }

    std::cout << "\n6. RAII handle returns the slot automatically\n";
    {
        ObjectPool<Tracer, 4> pool;
        {
            auto h = pool.make(99);
            std::cout << "   handle holds Tracer(" << h->id_ << "), in use " << pool.size() << '\n';
        }
        std::cout << "   handle destroyed, in use " << pool.size() << '\n';
    }

    std::cout << "\n7. The pool destructor cleans up after a leaked handle\n";
    {
        ObjectPool<Tracer, 4> pool;
        (void)pool.allocate(10);        // raw pointers, deliberately dropped
        (void)pool.allocate(20);
        std::cout << "   leaving scope with " << pool.size() << " live, never deallocated\n";
    }
    std::cout << "   (the two destructions above came from ~ObjectPool)\n";

    std::cout << "\n8. What the union buys, in bytes\n";
    {
        struct BothFields { Quote object; std::size_t next_free; };
        std::cout << "   sizeof(Quote)            = " << sizeof(Quote) << '\n'
                  << "   union slot               = " << ObjectPool<Quote, 4>::slot_size() << '\n'
                  << "   struct{T; size_t;} slot  = " << sizeof(BothFields) << '\n';
        struct SmallBoth { int object; std::size_t next_free; };
        std::cout << "   for T = int: union " << ObjectPool<int, 4>::slot_size()
                  << " vs struct " << sizeof(SmallBoth) << '\n';
    }
}
