// H2 — unions: one set of bytes, several possible types.
//
// H1 built objects into a raw std::byte array. A union is the other way to
// describe such storage: instead of "sizeof(T) bytes, aligned for T", you list
// the types you might store and let the compiler work out the size and
// alignment. In exchange you get two guarantees a byte array cannot give:
//
//   - every member starts at the same address, and a union is
//     pointer-interconvertible with each of its non-static data members, so
//     casting between union* and member* is well defined;
//   - reading the member you last wrote needs no std::launder.
//
// What you do NOT get is any idea of which member is live. A union has no tag.
// Tracking that is your job, and getting it wrong is silent corruption rather
// than a diagnostic. Everything below is about that one responsibility.
//
// The rules:
//   1. At most one member is active at a time — the one most recently written.
//   2. Reading an inactive member is undefined behaviour (one exception, §7).
//   3. If any member has a non-trivial constructor/destructor, the union's own
//      are deleted; you write them, and you destroy the active member by hand.
//   4. Switching to a non-trivial member means: destroy the old one, then
//      placement new the new one. H1's mechanics, applied here.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

// ===========================================================================
// A union with a non-trivial member. Because std::string has a non-trivial
// constructor and destructor, Value's are implicitly deleted — hence the
// hand-written pair. ~Value() is empty on purpose: only the owner knows which
// member is active, so only the owner can destroy it.
// ===========================================================================
union Value {
    std::int64_t integer;
    double       real;
    std::string  text;

    Value() noexcept : integer(0) {}
    ~Value() {}

    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
};

// The discipline of rule 3 and 4, wrapped up. This is a tagged union: the thing
// std::variant exists to stop you writing by hand.
class Tagged {
public:
    enum class Kind { Integer, Real, Text };

    Tagged() noexcept : kind_(Kind::Integer) { value_.integer = 0; }
    ~Tagged() { clear(); }

    // Copy and move would each need a switch over kind_ to pick the right
    // constructor. That boilerplate is the strongest practical argument for
    // std::variant, so it is left undone here rather than written out.
    Tagged(const Tagged&) = delete;
    Tagged& operator=(const Tagged&) = delete;

    void set(std::int64_t v) { clear(); value_.integer = v; kind_ = Kind::Integer; }
    void set(double v)       { clear(); value_.real    = v; kind_ = Kind::Real; }

    void set(std::string v) {
        clear();
        // The text member is not active yet, so it must be constructed, not
        // assigned. Assigning would run operator= on an object that does not
        // exist.
        std::construct_at(std::addressof(value_.text), std::move(v));
        kind_ = Kind::Text;
    }

    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    void print() const {
        std::cout << "   ";
        switch (kind_) {
            case Kind::Integer: std::cout << "integer " << value_.integer << '\n'; break;
            case Kind::Real:    std::cout << "real    " << value_.real    << '\n'; break;
            case Kind::Text:    std::cout << "text    \"" << value_.text << "\"\n"; break;
        }
    }

private:
    void clear() noexcept {
        // Only the non-trivial member needs destroying; the arithmetic ones do
        // not. Forgetting this line leaks the string's buffer.
        if (kind_ == Kind::Text) std::destroy_at(std::addressof(value_.text));
    }

    Value value_;
    Kind  kind_;
};

// ===========================================================================
// §6: the layout guarantee that G1's object pool relies on.
// ===========================================================================
struct Quote { std::uint64_t id; double price; };

union Slot {
    std::size_t next_free;
    Quote       object;
    Slot() noexcept : next_free(0) {}
    ~Slot() {}
};

// ===========================================================================
// §7: the one case where reading a different member is legal. Standard-layout
// structs sharing a common initial sequence may be inspected through any of
// them, so a tag at the front of every message can be read before the variant
// part is known. This is how wire protocols are decoded.
// ===========================================================================
struct Header { std::uint16_t type; std::uint32_t length; };
struct AddOrder { std::uint16_t type; std::uint32_t length; std::int64_t price; };
struct DeleteOrder { std::uint16_t type; std::uint32_t length; std::uint64_t id; };

union Message {
    Header      header;
    AddOrder    add;
    DeleteOrder del;
};

int main() {
    std::cout << "1. Size and alignment come from the widest member\n";
    {
        std::cout << "   sizeof(int64)=" << sizeof(std::int64_t)
                  << " sizeof(double)=" << sizeof(double)
                  << " sizeof(string)=" << sizeof(std::string) << '\n'
                  << "   sizeof(Value) = " << sizeof(Value)
                  << ", alignof(Value) = " << alignof(Value) << " (the maxima)\n";
    }

    std::cout << "\n2. Every member starts at the same address\n";
    {
        Value v;
        std::cout << "   &integer = " << static_cast<const void*>(std::addressof(v.integer)) << '\n'
                  << "   &real    = " << static_cast<const void*>(std::addressof(v.real)) << '\n'
                  << "   &text    = " << static_cast<const void*>(std::addressof(v.text)) << '\n';
    }

    std::cout << "\n3. Only the last member written is active\n";
    {
        Tagged t;
        t.set(std::int64_t{42});      t.print();
        t.set(3.5);                   t.print();
        t.set(std::string("a string long enough to need the heap")); t.print();
        t.set(std::int64_t{7});       t.print();   // destroys the string first
        std::cout << "   each set() destroyed the previous active member\n";
    }

    std::cout << "\n4. Reading an inactive member is undefined — do this instead\n";
    {
        const double d = 1.5;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));   // well defined
        std::cout << "   1.5 as bits via memcpy = 0x" << std::hex << bits << std::dec << '\n';
        std::cout << "   writing u.real then reading u.integer would be UB;\n"
                  << "   use memcpy, or std::bit_cast in C++20 (GCC 11+)\n";
    }

    std::cout << "\n5. Switching to a non-trivial member needs placement new\n";
    {
        Value v;                                 // integer is active
        std::construct_at(std::addressof(v.text), "built with construct_at");
        std::cout << "   text = \"" << v.text << "\"\n";
        std::destroy_at(std::addressof(v.text)); // required, or the buffer leaks
        std::cout << "   destroyed by hand; assigning v.integer now is fine again\n";
        v.integer = 1;
        std::cout << "   integer = " << v.integer << '\n';
    }

    std::cout << "\n6. Pointer-interconvertibility — what G1's pool used\n";
    {
        Slot slot;
        std::construct_at(std::addressof(slot.object), Quote{1, 101.5});
        Quote* q = std::addressof(slot.object);

        // Legal precisely because Slot is a union and object is a member of it.
        Slot* back = reinterpret_cast<Slot*>(q);
        std::cout << "   &slot == reinterpret_cast<Slot*>(&slot.object): "
                  << (back == std::addressof(slot) ? "yes" : "no") << '\n'
                  << "   that is how deallocate(T*) recovers the slot index\n";
        std::destroy_at(q);
    }

    std::cout << "\n7. The legal exception: a common initial sequence\n";
    {
        Message m;
        m.add = AddOrder{1, sizeof(AddOrder), 100'250};
        // add is active, but header shares its first two members, and all three
        // types are standard-layout — so reading m.header.type is well defined.
        std::cout << "   wrote m.add, read m.header.type = " << m.header.type
                  << ", length = " << m.header.length << '\n';
        std::cout << "   legal because Header and AddOrder are standard-layout\n"
                  << "   and share {uint16_t, uint32_t} as their initial members\n";
        if (m.header.type == 1)
            std::cout << "   dispatching on the tag: price = " << m.add.price << '\n';
    }

    std::cout << "\n8. std::variant does all of this safely\n";
    {
        using Safe = std::variant<std::int64_t, double, std::string>;
        Safe s = std::int64_t{42};
        s = std::string("variant tracks the active type itself");
        std::cout << "   index() = " << s.index() << ", value = "
                  << std::get<std::string>(s) << '\n';
        std::visit([](const auto& v) { std::cout << "   visited: " << v << '\n'; }, s);
        try {
            (void)std::get<double>(s);
        } catch (const std::bad_variant_access&) {
            std::cout << "   get<double> on a string threw bad_variant_access\n";
        }
        std::cout << "   sizeof(Value)  = " << sizeof(Value)
                  << " (no tag, you track it)\n"
                  << "   sizeof(Safe)   = " << sizeof(Safe)
                  << " (tag included, destruction and copying handled)\n";
        std::cout << "   Use a raw union only when that tag byte or the extra\n"
                  << "   indirection actually matters — as in G1, where the tag\n"
                  << "   lives in a shared bitset instead of per slot.\n";
    }
}
