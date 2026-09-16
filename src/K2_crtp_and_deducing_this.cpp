// K2 — static polymorphism: CRTP, and what C++23 replaces it with.
//
// CRTP exists to answer one question: how does a base class call into its
// derived class without a virtual function? The trick is to make the derived
// type a template argument of the base, so the base knows it statically:
//
//     template <typename Derived>
//     struct Base {
//         void interface() { static_cast<Derived&>(*this).impl(); }
//     };
//     struct Thing : Base<Thing> { void impl(); };
//
// That buys inlining and costs a vptr of nothing. It also costs more than most
// write-ups admit:
//
//   - Base<A> and Base<B> are UNRELATED types, so there is no common base to
//     store in a container or pass to a non-template function. CRTP is not a
//     substitute for virtual when you need runtime heterogeneity, only when you
//     do not;
//   - every derived class instantiates a fresh copy of the whole base (K3);
//   - the static_cast is unchecked. Write `struct B : Base<A>` and you get
//     undefined behaviour with no diagnostic;
//   - `static_cast<Derived&>(*this)` litters every method.
//
// C++23's "deducing this" (P0847) removes the reason for the pattern: a member
// function can take its own object as an explicit, deduced parameter, so the
// base does not need to be a template at all. GCC 10 has none of C++23, so that
// section is guarded by its feature-test macro and compiles when your toolchain
// catches up (GCC 14+, Clang 18+, MSVC 19.32+).

#include <chrono>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kIters = 50'000'000;

// ===========================================================================
// 1 — classic CRTP
// ===========================================================================
template <typename Derived>
class Shape {
public:
    // The public interface lives here once; the behaviour comes from Derived.
    [[nodiscard]] double area() const { return self().area_impl(); }
    [[nodiscard]] double scaled(double k) const { return self().area_impl() * k; }

    // Every CRTP base needs this, and it is the unchecked part: if Derived is
    // not actually the most-derived type, this cast is undefined behaviour.
    [[nodiscard]] const Derived& self() const { return static_cast<const Derived&>(*this); }
    [[nodiscard]] Derived& self() { return static_cast<Derived&>(*this); }
};

class Square : public Shape<Square> {
public:
    explicit Square(double s) noexcept : side_(s) {}
    [[nodiscard]] double area_impl() const noexcept { return side_ * side_; }
private:
    double side_;
};

class Circle : public Shape<Circle> {
public:
    explicit Circle(double r) noexcept : radius_(r) {}
    [[nodiscard]] double area_impl() const noexcept { return 3.14159265358979 * radius_ * radius_; }
private:
    double radius_;
};

// A safer CRTP base: constrain what Derived must provide, so a mistake is a
// named constraint failure instead of undefined behaviour at run time.
template <typename T>
concept HasAreaImpl = requires(const T& t) { { t.area_impl() } -> std::convertible_to<double>; };

template <typename Derived>
    requires HasAreaImpl<Derived>
class CheckedShape {
public:
    [[nodiscard]] double area() const { return static_cast<const Derived&>(*this).area_impl(); }
};

// ===========================================================================
// 2 — the virtual equivalent, for comparison
// ===========================================================================
class VShape {
public:
    virtual ~VShape() = default;
    [[nodiscard]] virtual double area() const = 0;
};
class VSquare final : public VShape {
public:
    explicit VSquare(double s) noexcept : side_(s) {}
    [[nodiscard]] double area() const override { return side_ * side_; }
private:
    double side_;
};
class VCircle final : public VShape {
public:
    explicit VCircle(double r) noexcept : radius_(r) {}
    [[nodiscard]] double area() const override { return 3.14159265358979 * radius_ * radius_; }
private:
    double radius_;
};

// ===========================================================================
// 3 — CRTP as a mixin: adding operations, not dispatching behaviour.
//
// NOTE FIRST: in C++20 you would not write this. A single defaulted <=> gives
// you every relational operator, with correct symmetry, no base class, and none
// of the costs below. Version20 further down is the code you should actually
// ship; Comparable is here because the PATTERN is worth understanding and still
// turns up in pre-C++20 codebases.
//
// Why the operators are `friend` rather than members or free templates — four
// reasons, in order of how much they matter:
//
//   1. A friend DEFINED INSIDE A CLASS TEMPLATE is not itself a template. Each
//      instantiation of Comparable<D> emits an ordinary function taking exactly
//      (const D&, const D&). `nm` shows `operator!=(Version const&, Version
//      const&)`, with no template arguments. You write it once, generically, and
//      get a concrete overload per opted-in type.
//   2. The obvious alternative — hoisting it to namespace scope, where it would
//      have to be `template <typename D> bool operator!=(const D&, const D&)` —
//      is catastrophically greedy: it matches ANY two same-typed operands in the
//      whole program. It rarely hijacks types that have their own operator, but
//      it wrecks diagnostics for every type that does not: a clean
//          error: no match for 'operator!=' (operand types are 'NoEq' and 'NoEq')
//      becomes an instantiation backtrace pointing inside YOUR mixin, for a type
//      that never heard of it.
//   3. Hidden friend: the name is not injected into the enclosing namespace, so
//      it is findable only by ADL. `::operator!=(a, b)` does not compile, while
//      `a != b` does. Smaller overload sets, no namespace pollution, and the
//      compiler never considers it for unrelated calls.
//   4. Symmetry. A member operator takes the left operand as the implicit object,
//      and no USER-DEFINED conversion is applied there. With a member `operator<`,
//      `1 < M(2)` fails; with a friend it compiles. Members give asymmetric
//      comparison, which is almost never what a relational operator should do.
//
// Being precise: friend is not strictly REQUIRED. A member version compiles,
// because derived-to-base on the implicit object is a standard conversion. It
// just loses reason 4 and adds a static_cast to every body.
// ===========================================================================
template <typename Derived>
struct Comparable {
    [[nodiscard]] friend bool operator!=(const Derived& a, const Derived& b) { return !(a == b); }
    [[nodiscard]] friend bool operator> (const Derived& a, const Derived& b) { return b < a; }
    [[nodiscard]] friend bool operator<=(const Derived& a, const Derived& b) { return !(b < a); }
    [[nodiscard]] friend bool operator>=(const Derived& a, const Derived& b) { return !(a < b); }
};

struct Version : Comparable<Version> {
    int major{}, minor{};
    [[nodiscard]] friend bool operator==(const Version& a, const Version& b) {
        return a.major == b.major && a.minor == b.minor;
    }
    [[nodiscard]] friend bool operator<(const Version& a, const Version& b) {
        return a.major != b.major ? a.major < b.major : a.minor < b.minor;
    }
};

// The C++20 answer: one line replaces the whole mixin. != is rewritten from ==,
// and < > <= >= are all synthesised from <=>.
struct Version20 {
    int major{}, minor{};
    auto operator<=>(const Version20&) const = default;
};

// A counting mixin: each Derived gets its own counter, because each Derived
// instantiates its own base. That is normally the bloat complaint; here it is
// exactly the feature.
template <typename Derived>
struct Counted {
    Counted() noexcept { ++live; }
    Counted(const Counted&) noexcept { ++live; }
    ~Counted() { --live; }
    static inline int live = 0;
};
struct Widget : Counted<Widget> {};
struct Gadget : Counted<Gadget> {};

// ===========================================================================
// 4 — the problem deducing this really fixes: overload duplication.
// Pre-C++23 a getter that should work on const, non-const, lvalue and rvalue
// objects needs FOUR near-identical bodies.
// ===========================================================================
class Holder {
public:
    explicit Holder(std::string v) : value_(std::move(v)) {}
    [[nodiscard]] std::string&        get() &       { return value_; }
    [[nodiscard]] const std::string&  get() const&  { return value_; }
    [[nodiscard]] std::string&&       get() &&      { return std::move(value_); }
    [[nodiscard]] const std::string&& get() const&& { return std::move(value_); }
private:
    std::string value_;
};

#if defined(__cpp_explicit_this_parameter)
// ===========================================================================
// 5 — C++23. Note what is NOT here: no template parameter on the class, no
// static_cast, no self(). Base is one ordinary type that every derived class
// shares, so it can also be used non-generically.
// ===========================================================================
struct Shape23 {
    template <typename Self>
    [[nodiscard]] double area(this const Self& self) { return self.area_impl(); }
    template <typename Self>
    [[nodiscard]] double scaled(this const Self& self, double k) { return self.area_impl() * k; }
};
struct Square23 : Shape23 {
    double side;
    [[nodiscard]] double area_impl() const noexcept { return side * side; }
};

// And the four overloads above collapse into one.
class Holder23 {
public:
    explicit Holder23(std::string v) : value_(std::move(v)) {}
    template <typename Self>
    [[nodiscard]] auto&& get(this Self&& self) { return std::forward<Self>(self).value_; }
private:
    std::string value_;
};
#endif

// ---------------------------------------------------------------------------
template <typename F>
[[nodiscard]] double time_ns(F&& f, std::size_t iters) {
    const auto t0 = Clock::now();
    f();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(iters);
}

volatile double g_sink = 0.0;

}  // namespace

int main() {
    std::printf("1. CRTP: the base calls into the derived class with no vtable\n");
    {
        Square sq{3.0};
        Circle ci{1.0};
        std::printf("   Square{3}.area()  = %.4f   (via Shape<Square>::area)\n", sq.area());
        std::printf("   Circle{1}.area()  = %.4f   (via Shape<Circle>::area)\n", ci.area());
        std::printf("   sizeof(Square) = %zu, sizeof(VSquare) = %zu  <- the vptr is the difference\n",
                    sizeof(Square), sizeof(VSquare));
        std::printf("   Shape<Square> and Shape<Circle> are unrelated types:\n");
        std::printf("     is_base_of<Shape<Square>, Circle> = %s\n",
                    std::is_base_of_v<Shape<Square>, Circle> ? "true" : "false");
        std::printf("   so there is no container that can hold both. That is the real cost.\n");
    }

    std::printf("\n2. What the indirection is worth\n");
    {
        Square sq{3.0};
        const double crtp_ns = time_ns([&] {
            double acc = 0;
            for (std::size_t i = 0; i < kIters; ++i) acc += sq.area();
            g_sink = acc;
        }, kIters);

        // A vector of mixed derived types defeats devirtualisation, which is the
        // situation you are actually in when you reach for virtual.
        std::vector<std::unique_ptr<VShape>> shapes;
        for (int i = 0; i < 2; ++i) {
            shapes.emplace_back(std::make_unique<VSquare>(3.0));
            shapes.emplace_back(std::make_unique<VCircle>(1.0));
        }
        const double virt_ns = time_ns([&] {
            double acc = 0;
            for (std::size_t i = 0; i < kIters; ++i) acc += shapes[i & 3]->area();
            g_sink = acc;
        }, kIters);

        std::printf("   CRTP    (static, inlinable): %6.2f ns per call\n", crtp_ns);
        std::printf("   virtual (mixed types)      : %6.2f ns per call\n", virt_ns);
        std::printf("   ratio %.1fx — but note the CRTP loop inlines to almost nothing,\n",
                    crtp_ns > 0 ? virt_ns / crtp_ns : 0.0);
        std::printf("   so this measures inlining, not dispatch. Virtual costs an indirect\n");
        std::printf("   call AND blocks the optimiser from seeing through it.\n");
    }

    std::printf("\n3. CRTP as a mixin: operations, not dispatch\n");
    {
        // Note the empty first initialiser: deriving from Comparable<Version>
        // makes Version an aggregate WITH A BASE, so brace initialisation needs
        // a slot for it. A small, real cost of the pattern.
        const Version a{{}, 1, 2}, b{{}, 1, 3};
        std::printf("   Version{1,2} <  {1,3} = %s   (hand-written)\n", (a < b) ? "true" : "false");
        std::printf("   Version{1,2} >= {1,3} = %s   (supplied by Comparable<Version>)\n",
                    (a >= b) ? "true" : "false");

        // Same results, one line of code, no base class, no brace-init gotcha.
        const Version20 c{1, 2}, d{1, 3};
        const bool agree = (a < b) == (c < d) && (a >= b) == (c >= d)
                        && (a != b) == (c != d) && (a <= b) == (c <= d);
        std::printf("   Version20 with `auto operator<=>(const Version20&) const = default;`\n");
        std::printf("     gives <, >, <=, >=, ==, != and agrees with the mixin: %s\n",
                    agree ? "yes" : "NO");
        std::printf("     and note Version20{1,2} needs no empty base initialiser\n");
        {
            Widget w1, w2;
            Gadget g1;
            std::printf("   Counted<Widget>::live = %d, Counted<Gadget>::live = %d\n",
                        Widget::live, Gadget::live);
            std::printf("   separate counters, because each Derived gets its own base\n");
            (void)w1; (void)w2; (void)g1;
        }
        std::printf("   after scope: Widget %d, Gadget %d\n", Widget::live, Gadget::live);
    }

    std::printf("\n4. The overload duplication deducing this removes\n");
    {
        Holder h{"value"};
        std::printf("   Holder::get() needs 4 overloads (&, const&, &&, const&&)\n");
        std::printf("   non-const lvalue -> \"%s\"\n", h.get().c_str());
        std::printf("   rvalue           -> \"%s\" (moved out)\n",
                    std::string(Holder{"temp"}.get()).c_str());
    }

    std::printf("\n5. C++23 deducing this\n");
#if defined(__cpp_explicit_this_parameter)
    {
        Square23 s{3.0};
        std::printf("   available (__cpp_explicit_this_parameter = %ld)\n",
                    static_cast<long>(__cpp_explicit_this_parameter));
        std::printf("   Square23{3}.area() = %.4f, with no CRTP template parameter\n", s.area());
        Holder23 h{"value"};
        std::printf("   Holder23::get() is ONE function covering all four cases: \"%s\"\n",
                    h.get().c_str());
    }
#else
    std::printf("   NOT AVAILABLE on this toolchain (GCC %d.%d, no C++23 support at all).\n",
                __GNUC__, __GNUC_MINOR__);
    std::printf("   The code is in the source behind #if __cpp_explicit_this_parameter.\n\n");
    std::printf("     struct Shape23 {                       // not a template\n");
    std::printf("         template <typename Self>\n");
    std::printf("         double area(this const Self& self) // the object, deduced\n");
    std::printf("         { return self.area_impl(); }\n");
    std::printf("     };\n");
    std::printf("     struct Square23 : Shape23 { ... };     // no Shape23<Square23>\n\n");
    std::printf("   What that changes:\n");
    std::printf("     - Shape23 is ONE type, so derived classes share a base again and\n");
    std::printf("       the container problem from section 1 goes away\n");
    std::printf("     - no static_cast, so the silent undefined behaviour is gone\n");
    std::printf("     - the four-overload getter collapses to one forwarding function\n");
    std::printf("     - lambdas can recurse without the Y-combinator trick\n");
    std::printf("   Needs GCC 14+, Clang 18+, or MSVC 19.32+.\n");
#endif

    std::printf("\n6. Choosing\n");
    std::printf("   - need runtime heterogeneity (a container of mixed types)?  virtual.\n");
    std::printf("   - need the call inlined and the type is known at compile time?  CRTP,\n");
    std::printf("     or in C++20 just a constrained free function template, which needs\n");
    std::printf("     no base class at all and is usually the better answer.\n");
    std::printf("   - only adding operations from a few primitives?  mixin CRTP is fine,\n");
    std::printf("     and remains fine in C++23 — but for COMPARISON specifically,\n");
    std::printf("     C++20's defaulted <=> replaces the mixin outright.\n");
    std::printf("   - on C++23?  deducing this, and stop writing the pattern.\n");
    std::printf("   Every CRTP instantiation duplicates the base for each Derived — see K3\n");
    std::printf("   for what that does to the binary.\n");
}
