// H1 — placement new: constructing objects into memory you already own.
//
// Ordinary `new T(args)` does two separate jobs: it acquires memory, then it
// constructs a T in it. Placement new does only the second. That separation is
// what every pool, small-buffer optimisation, optional and variant is built on.
//
//     new T(args)            -> allocate, then construct
//     new (address) T(args)  -> construct only, at an address you supply
//
// Three rules follow, and they are the whole topic:
//
//   1. Placement new never allocates, so it never throws bad_alloc, and there
//      is nothing to free. Calling `delete` on the result is undefined.
//   2. Nothing destroys the object for you. You must call `p->~T()` yourself,
//      exactly once, before the storage is reused or goes away.
//   3. The storage must be big enough AND correctly aligned for T.
//
// This file builds that up one step at a time, using a templated block of raw
// memory. The union-based way of laying out such a block is H2's subject; here
// the storage is always a plain `std::byte` array.

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// ===========================================================================
// Storage for exactly one T — with no T in it yet.
// ===========================================================================
template <typename T>
class RawStorage {
public:
    // Hands back the address to build at. No object lives here yet, so this is
    // deliberately `void*`: there is nothing of type T to point to.
    [[nodiscard]] void* address() noexcept { return bytes_; }

    template <typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        // The returned pointer is the one to use. It names the new object;
        // `bytes_` merely names the storage underneath it.
        return new (bytes_) T(std::forward<Args>(args)...);
    }

    // Only valid between construct() and destroy(). std::launder tells the
    // compiler "an object of type T really does live at these bytes now" —
    // without it, reading T through a pointer derived from bytes_ is UB.
    [[nodiscard]] T* get() noexcept { return std::launder(reinterpret_cast<T*>(bytes_)); }

    void destroy() noexcept { get()->~T(); }

    static constexpr std::size_t size()      noexcept { return sizeof(T); }
    static constexpr std::size_t alignment() noexcept { return alignof(T); }

private:
    alignas(T) std::byte bytes_[sizeof(T)];
};

// ===========================================================================
// A templated block of storage for N objects, none of them constructed yet.
// Indices are managed by the caller, which is the point: the block knows about
// bytes, not about which slots are live.
// ===========================================================================
template <typename T, std::size_t N>
class RawBlock {
    static_assert(N > 0);
public:
    // Not [[nodiscard]]: the index already identifies the object, and at()
    // retrieves it, so discarding the pointer is the normal case.
    template <typename... Args>
    T* construct(std::size_t index, Args&&... args) {
        return new (slot(index)) T(std::forward<Args>(args)...);
    }

    [[nodiscard]] T* at(std::size_t index) noexcept {
        return std::launder(reinterpret_cast<T*>(slot(index)));
    }

    void destroy(std::size_t index) noexcept { at(index)->~T(); }

    static constexpr std::size_t count()      noexcept { return N; }
    static constexpr std::size_t byte_size()  noexcept { return sizeof(T) * N; }

private:
    [[nodiscard]] std::byte* slot(std::size_t index) noexcept {
        return bytes_ + index * sizeof(T);
    }
    alignas(T) std::byte bytes_[sizeof(T) * N];
};

// Construct every element, and on failure destroy exactly those already built.
// This is the pattern std::uninitialized_fill and vector's growth both use.
template <typename T, std::size_t N, typename... Args>
void construct_all(RawBlock<T, N>& block, const Args&... args) {
    std::size_t built = 0;
    try {
        for (; built < N; ++built) block.construct(built, args...);
    } catch (...) {
        while (built-- > 0) block.destroy(built);   // unwind, newest first
        throw;
    }
}

// ---------------------------------------------------------------------------

struct Tracer {
    int id_;
    explicit Tracer(int id) : id_(id) { std::cout << "      Tracer(" << id_ << ") constructed\n"; }
    ~Tracer() { std::cout << "      Tracer(" << id_ << ") destroyed\n"; }
};

// No default constructor, and owns heap memory — so forgetting ~T() leaks.
struct Message {
    std::string text_;
    int         priority_;
    Message(std::string text, int priority) : text_(std::move(text)), priority_(priority) {}
};

// Throws on the Nth construction, to exercise construct_all's unwinding.
struct Fragile {
    static int live;
    static int throw_on;
    int id_;
    Fragile() : id_(live) {
        if (live == throw_on) throw std::runtime_error("Fragile ctor failed");
        ++live;
        std::cout << "      Fragile(" << id_ << ") constructed\n";
    }
    ~Fragile() { --live; std::cout << "      Fragile(" << id_ << ") destroyed\n"; }
};
int Fragile::live = 0;
int Fragile::throw_on = -1;

struct alignas(32) Wide { double v[4]; };

int main() {
    std::cout << "1. Storage without an object\n";
    {
        RawStorage<Tracer> storage;
        std::cout << "   sizeof(Tracer) = " << RawStorage<Tracer>::size()
                  << ", alignof = " << RawStorage<Tracer>::alignment() << '\n';
        std::cout << "   the RawStorage object itself is " << sizeof(storage)
                  << " bytes at " << static_cast<const void*>(storage.address()) << '\n'
                  << "   declared — but no constructor ran (nothing printed above)\n";
    }

    std::cout << "\n2. construct -> use -> destroy, explicitly\n";
    {
        RawStorage<Tracer> storage;
        Tracer* t = storage.construct(1);
        std::cout << "   using Tracer(" << t->id_ << ")\n";
        storage.destroy();                 // required: nothing else will do it
        std::cout << "   destroy() called by hand\n";
    }

    std::cout << "\n3. Placement new returns the pointer to use\n";
    {
        RawStorage<Message> storage;
        Message* m = storage.construct("fill or kill", 7);
        std::cout << "   construct() returned " << static_cast<const void*>(m) << '\n'
                  << "   storage address is  " << static_cast<const void*>(storage.address()) << '\n'
                  << "   same address, but only the first names an object\n";
        std::cout << "   get() agrees: " << (storage.get() == m ? "yes" : "no")
                  << ", text = \"" << m->text_ << "\"\n";
        storage.destroy();                 // frees the std::string's buffer
    }

    std::cout << "\n4. Constructor arguments are forwarded, so no default ctor is needed\n";
    {
        RawStorage<Message> storage;
        Message* m = storage.construct(std::string("limit"), 3);
        std::cout << "   Message{\"" << m->text_ << "\", " << m->priority_ << "}\n";
        storage.destroy();
    }

    std::cout << "\n5. The same bytes can host a second object after the first dies\n";
    {
        RawStorage<Tracer> storage;
        Tracer* first = storage.construct(10);
        const void* addr = first;
        storage.destroy();
        Tracer* second = storage.construct(20);
        std::cout << "   same address reused: " << (addr == second ? "yes" : "no") << '\n';
        storage.destroy();
    }

    std::cout << "\n6. A block of N slots, partially constructed\n";
    {
        RawBlock<Tracer, 5> block;
        std::cout << "   block holds " << RawBlock<Tracer, 5>::byte_size()
                  << " bytes for " << RawBlock<Tracer, 5>::count() << " slots, 0 objects\n";
        for (std::size_t i = 0; i < 3; ++i) block.construct(i, static_cast<int>(100 + i));
        std::cout << "   slots 3 and 4 were never constructed — so they must not be destroyed\n";
        for (std::size_t i = 3; i-- > 0;) block.destroy(i);
    }

    std::cout << "\n7. Exception safety: destroy only what was built\n";
    {
        RawBlock<Fragile, 5> block;
        Fragile::live = 0;
        Fragile::throw_on = 3;             // the 4th construction throws
        try {
            construct_all(block);
            std::cout << "   ERROR: should have thrown\n";
        } catch (const std::runtime_error& e) {
            std::cout << "   caught: " << e.what() << '\n';
        }
        std::cout << "   live objects after unwinding: " << Fragile::live << " (must be 0)\n";
        Fragile::throw_on = -1;
    }

    std::cout << "\n8. Alignment is part of the contract\n";
    {
        RawStorage<Wide> storage;
        Wide* w = storage.construct();
        const auto addr = reinterpret_cast<std::uintptr_t>(w);
        std::cout << "   alignof(Wide) = " << alignof(Wide)
                  << ", address % " << alignof(Wide) << " = " << (addr % alignof(Wide))
                  << " (0 means correctly aligned)\n";
        std::cout << "   alignas(T) on the byte array is what guarantees that\n";
        storage.destroy();
    }

    std::cout << "\n9. The C++20 spellings\n";
    {
        RawStorage<Message> storage;
        // construct_at is placement new with a typed pointer, and is usable in
        // constant expressions; destroy_at is the matching explicit destructor.
        Message* m = std::construct_at(reinterpret_cast<Message*>(storage.address()),
                                       std::string("constructed via construct_at"), 1);
        std::cout << "   " << m->text_ << '\n';
        std::destroy_at(m);
        std::cout << "   destroy_at() instead of m->~Message()\n";

        RawBlock<Tracer, 3> block;
        for (std::size_t i = 0; i < 3; ++i) block.construct(i, static_cast<int>(i));
        std::destroy_n(block.at(0), 3);    // destroys 3 consecutive objects
        std::cout << "   destroy_n() destroyed all three\n";
    }

    std::cout << "\n10. What not to do (each line below is undefined behaviour)\n";
    {
        RawStorage<Tracer> storage;
        Tracer* t = storage.construct(99);

        // delete t;              // never: the storage did not come from new
        // storage.destroy();
        // storage.destroy();     // never: one destructor call per object
        // (void)storage.get();   // never after destroy(): no object lives there

        std::cout << "   see the commented-out lines in the source\n";
        storage.destroy();        // the one correct call
        (void)t;
    }
}
