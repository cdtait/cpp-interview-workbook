// K1 — template mechanics: deduction, forwarding, variadics, traits.
//
// Templates are usually taught as syntax. The parts that actually bite are
// rules, and they are few:
//
//   1. DEDUCTION strips things. A by-value parameter drops const, volatile and
//      references; arrays and functions decay to pointers. A T& or T&&
//      parameter does not strip, which is why the two behave so differently.
//   2. A T&& on a DEDUCED parameter is not an rvalue reference, it is a
//      forwarding reference, and reference collapsing decides what it becomes.
//      T&& on a non-deduced parameter really is an rvalue reference.
//   3. std::forward is a conditional cast that exists only to undo the fact
//      that a named rvalue reference is itself an lvalue.
//   4. Pack expansion repeats a PATTERN, once per element. Fold expressions are
//      sugar for the common case, and left and right folds differ for any
//      operator that is not associative.
//
// Everything below prints what it deduced, so the rules are observable rather
// than asserted.

#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// Prints the name of a type by reading the compiler's own signature for this
// function. No RTTI, no demangling, and it works on incomplete types.
template <typename T>
[[nodiscard]] constexpr std::string_view type_name() {
    std::string_view p = __PRETTY_FUNCTION__;
    const auto start = p.find("T = ") + 4;
    auto end = p.find(';', start);
    if (end == std::string_view::npos) end = p.find(']', start);
    return p.substr(start, end - start);
}

// libstdc++ spells std::string as std::__cxx11::basic_string<char>, which buries
// the point under noise. Shorten the common ones for display only.
[[nodiscard]] std::string pretty(std::string_view name) {
    std::string out(name);
    const std::pair<std::string_view, std::string_view> swaps[] = {
        {"std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >", "std::string"},
        {"std::__cxx11::basic_string<char>", "std::string"},
        {"std::__cxx11::", "std::"},
    };
    for (const auto& [from, to] : swaps)
        for (std::size_t at = out.find(from); at != std::string::npos; at = out.find(from))
            out.replace(at, from.size(), to);
    return out;
}

// ===========================================================================
// 1 — what deduction keeps and what it throws away
// ===========================================================================
template <typename T> void by_value(T)        { std::printf("      %-20s T = %s\n", "by_value(T)", pretty(type_name<T>()).c_str()); }
template <typename T> void by_lref(T&)        { std::printf("      %-20s T = %s\n", "by_lref(T&)", pretty(type_name<T>()).c_str()); }
template <typename T> void by_clref(const T&) { std::printf("      %-20s T = %s\n", "by_clref(const T&)", pretty(type_name<T>()).c_str()); }

// A deduced T&& is a FORWARDING reference: it binds to anything, and T carries
// the value category. This is the single most misread declaration in C++.
template <typename T> void by_fwd(T&& arg) {
    std::printf("%-20s T = %-24s decltype(arg) = %s\n", "by_fwd(T&&)",
                pretty(type_name<T>()).c_str(),
                pretty(type_name<decltype(arg)>()).c_str());
}

// ===========================================================================
// 3 — why std::forward exists
// ===========================================================================
void sink(const std::string&) { std::printf("      -> sink(const&)  [copy]\n"); }
void sink(std::string&&)      { std::printf("      -> sink(&&)      [move]\n"); }

template <typename T> void relay_plain(T&& v)   { sink(v); }                    // always lvalue
template <typename T> void relay_forward(T&& v) { sink(std::forward<T>(v)); }   // preserves

// ===========================================================================
// 4 — packs and folds
// ===========================================================================
template <typename... Ts>
[[nodiscard]] constexpr auto sum_fold(Ts... vs) { return (vs + ...); }           // unary right fold

// For a non-associative operator the direction is visible in the answer:
//   left  : ((a - b) - c)
//   right : (a - (b - c))
template <typename... Ts> [[nodiscard]] constexpr auto fold_left(Ts... vs)  { return (... - vs); }
template <typename... Ts> [[nodiscard]] constexpr auto fold_right(Ts... vs) { return (vs - ...); }

// Pack expansion repeats the whole pattern, not just the name. Here the pattern
// is "print one element", and the comma fold sequences them.
template <typename... Ts>
void print_all(const Ts&... vs) {
    std::printf("      %zu args:", sizeof...(Ts));
    ((void)vs, ...);          // the values are not needed here, only their types
    ((std::printf(" %s", pretty(type_name<Ts>()).c_str())), ...);
    std::putchar('\n');
}

// A fold over && short-circuits, which a recursive version would not do for free.
template <typename... Ts>
[[nodiscard]] constexpr bool all_arithmetic() { return (std::is_arithmetic_v<Ts> && ...); }

// Indexing a pack needs an index_sequence: packs have no operator[].
template <typename Tuple, std::size_t... Is>
void print_tuple_impl(const Tuple& t, std::index_sequence<Is...>) {
    ((std::printf(Is == 0 ? "%d" : ", %d", static_cast<int>(std::get<Is>(t)))), ...);
}
template <typename... Ts>
void print_tuple(const std::tuple<Ts...>& t) {
    std::printf("      (");
    print_tuple_impl(t, std::index_sequence_for<Ts...>{});
    std::printf(")\n");
}

// ===========================================================================
// 5 — detecting a capability, four generations of the same idea
// ===========================================================================

// (a) void_t detection idiom — C++17 and the classic interview answer.
template <typename, typename = void> struct has_size_voidt : std::false_type {};
template <typename T>
struct has_size_voidt<T, std::void_t<decltype(std::declval<const T&>().size())>> : std::true_type {};

// (b) enable_if on the return type — the C++11 way, and why error messages used
//     to be unreadable: the constraint lives in the signature as a substitution.
template <typename T>
[[nodiscard]] auto describe_enable_if(const T&)
    -> std::enable_if_t<has_size_voidt<T>::value, const char*> { return "sized (enable_if)"; }
template <typename T>
[[nodiscard]] auto describe_enable_if(const T&)
    -> std::enable_if_t<!has_size_voidt<T>::value, const char*> { return "plain (enable_if)"; }

// (c) if constexpr — one function, no overload set, discarded branch not
//     instantiated. Much simpler when you do not need overload resolution.
template <typename T>
[[nodiscard]] const char* describe_if_constexpr(const T&) {
    if constexpr (has_size_voidt<T>::value) return "sized (if constexpr)";
    else                                    return "plain (if constexpr)";
}

// (d) concepts — C++20. The constraint is named, reusable, and subsumption
//     picks the more specific overload without any tie-break trickery.
template <typename T>
concept Sized = requires(const T& t) { { t.size() } -> std::convertible_to<std::size_t>; };

template <typename T>          [[nodiscard]] const char* describe(const T&) { return "plain (concept)"; }
template <Sized T>             [[nodiscard]] const char* describe(const T&) { return "sized (concept)"; }

// ===========================================================================
// 6 — a small piece of real machinery: a type-safe, allocation-free apply
// ===========================================================================
template <typename F, typename... Args>
[[nodiscard]] constexpr decltype(auto) call_forwarding(F&& f, Args&&... args)
    noexcept(std::is_nothrow_invocable_v<F, Args...>) {
    static_assert(std::is_invocable_v<F, Args...>, "arguments do not match the callable");
    return std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
}

struct Probe {
    void operator()(std::string&&)      const { std::printf("      callable saw an rvalue\n"); }
    void operator()(const std::string&) const { std::printf("      callable saw an lvalue\n"); }
};

// ===========================================================================
// 7 — CTAD and a deduction guide
// ===========================================================================
template <typename T, std::size_t N>
struct Fixed {
    std::array<T, N> data;
};
// Without this guide, Fixed{1, 2, 3} does not compile: N is not deducible from
// a braced list on its own.
template <typename... Ts>
Fixed(Ts...) -> Fixed<std::common_type_t<Ts...>, sizeof...(Ts)>;

}  // namespace

int main() {
    std::printf("1. Deduction: by value strips, by reference does not\n");
    {
        const int  ci = 1;
        int        i  = 2;
        const int  arr[3]{};
        std::printf("   argument: const int lvalue\n");
        by_value(ci);          // const dropped
        by_lref(ci);           // const kept, it is part of T
        by_clref(ci);
        std::printf("   argument: int array of 3\n");
        by_value(arr);         // decays to const int*
        by_lref(arr);          // stays an array
        (void)i;
    }

    std::printf("\n2. A deduced T&& is a forwarding reference; collapsing decides what it is\n");
    {
        std::string s = "hello";
        const std::string cs = "hello";
        std::printf("   lvalue:        "); by_fwd(s);    // T = std::string&  -> T&& collapses to &
        std::printf("   const lvalue:  "); by_fwd(cs);   // T = const std::string&
        std::printf("   rvalue:        "); by_fwd(std::string{"tmp"});  // T = std::string
        std::printf("   collapsing:  & &  -> &    & && -> &    && &  -> &    && && -> &&\n");
        std::printf("   so only a genuine rvalue leaves T as a non-reference type.\n");
    }

    std::printf("\n3. std::forward: a named rvalue reference is itself an lvalue\n");
    {
        std::string s = "abc";
        std::printf("   relay_plain  (rvalue arg): "); relay_plain(std::string{"x"});
        std::printf("   relay_forward(rvalue arg): "); relay_forward(std::string{"x"});
        std::printf("   relay_forward(lvalue arg): "); relay_forward(s);
        std::printf("   Without forward the move overload is unreachable, silently.\n");
    }

    std::printf("\n4. Packs and folds\n");
    {
        std::printf("      sum_fold(1, 2, 3, 4)          = %d\n", sum_fold(1, 2, 3, 4));
        std::printf("      fold_left (100, 20, 3)        = %d   ((100-20)-3)\n", fold_left(100, 20, 3));
        std::printf("      fold_right(100, 20, 3)        = %d   (100-(20-3))\n", fold_right(100, 20, 3));
        print_all(1, 2.5, 'c', std::string{});
        std::printf("      all_arithmetic<int,double>()  = %s\n", all_arithmetic<int, double>() ? "true" : "false");
        std::printf("      all_arithmetic<int,string>()  = %s\n", all_arithmetic<int, std::string>() ? "true" : "false");
        print_tuple(std::tuple{10, 20, 30});
    }

    std::printf("\n5. Constraining, four generations, same answer\n");
    {
        const std::vector<int> v{1, 2, 3};
        const int n = 7;
        std::printf("   %-24s vector: %-22s int: %s\n", "void_t + enable_if",
                    describe_enable_if(v), describe_enable_if(n));
        std::printf("   %-24s vector: %-22s int: %s\n", "if constexpr",
                    describe_if_constexpr(v), describe_if_constexpr(n));
        std::printf("   %-24s vector: %-22s int: %s\n", "concepts",
                    describe(v), describe(n));
        std::printf("   Concepts win on the error message, not the result: an unsatisfied\n");
        std::printf("   constraint names itself, where enable_if reports 'no matching function'.\n");
    }

    std::printf("\n6. Perfect forwarding through a generic call\n");
    {
        std::string s = "abc";
        call_forwarding(Probe{}, std::string{"tmp"});
        call_forwarding(Probe{}, s);
        std::printf("      is_nothrow_invocable_v<Probe, std::string&&> = %s\n",
                    std::is_nothrow_invocable_v<Probe, std::string&&> ? "true" : "false");
    }

    std::printf("\n7. CTAD with a deduction guide\n");
    {
        Fixed f{1, 2, 3, 4};
        std::printf("      Fixed{1,2,3,4} deduced as %s\n",
                    pretty(type_name<decltype(f)>()).c_str());
        std::printf("      without the guide this line does not compile at all\n");
    }

    std::printf("\n8. The rules worth memorising\n");
    std::printf("   - by value deduces away const/volatile/& and decays arrays and functions\n");
    std::printf("   - T&& is forwarding ONLY when T is deduced in that same declaration;\n");
    std::printf("     void f(std::string&&) and template<class T> void f(T&&) are unrelated\n");
    std::printf("   - forward<T> where T came from a forwarding reference; move where you own it\n");
    std::printf("   - prefer a fold to a recursive pack: fewer instantiations (see K3)\n");
    std::printf("   - prefer concepts to enable_if for anything a colleague has to read\n");
}
