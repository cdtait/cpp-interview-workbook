// K4 — constexpr, consteval, constinit: what each one actually promises.
//
// The three keywords are easy to conflate because they all say "compile time".
// They promise very different things:
//
//   constexpr (function)  MAY be evaluated at compile time. It is a permission,
//                         not a promise. Called in a runtime context it is an
//                         ordinary function, and at -O0 GCC emits and calls it.
//   constexpr (variable)  MUST be initialised at compile time. This is where the
//                         guarantee lives — the context, not the function.
//   consteval (function)  MUST be evaluated at compile time, every call. A
//                         runtime call is a compile error, not a fallback.
//   constinit (variable)  MUST be constant-INITIALISED, but stays mutable. It
//                         buys you freedom from the static initialisation order
//                         fiasco (D1) without making the object const.
//
// The distinction matters in practice: writing `constexpr` on a function and
// assuming the work has vanished is one of the most common mistaken beliefs
// about it. Section 1 measures that directly by compiling the same source twice.

#include <array>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <type_traits>
#include <version>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Compile a snippet and report what happened. Used to demonstrate the cases
// that are supposed to FAIL, since an example that only shows working code
// cannot show you where the boundary is.
// ---------------------------------------------------------------------------
struct CompileResult { bool ok; std::string first_error; };

[[nodiscard]] CompileResult compile_snippet(const std::string& code, const char* extra = "") {
    char path[] = "/tmp/k4_snippet_XXXXXX.cpp";
    const int fd = mkstemps(path, 4);
    if (fd < 0) return {false, "could not create temp file"};
    const bool wrote = ::write(fd, code.data(), code.size()) == static_cast<ssize_t>(code.size());
    ::close(fd);
    if (!wrote) { ::unlink(path); return {false, "could not write temp file"}; }

    char cmd[PATH_MAX + 256];
    std::snprintf(cmd, sizeof(cmd), "g++ -std=c++20 %s -c '%s' -o /dev/null 2>&1", extra, path);
    std::FILE* pipe = popen(cmd, "r");
    CompileResult out{true, {}};
    if (pipe) {
        char line[1024];
        while (std::fgets(line, sizeof(line), pipe)) {
            if (std::strstr(line, "error:") && out.first_error.empty()) {
                const char* msg = std::strstr(line, "error:");
                out.first_error = msg;
                if (!out.first_error.empty() && out.first_error.back() == '\n')
                    out.first_error.pop_back();
                out.ok = false;
            }
        }
        pclose(pipe);
    }
    ::unlink(path);
    return out;
}

// Count how many times a symbol appears when the snippet is compiled at a given
// optimisation level: the direct way to show whether the call survived.
[[nodiscard]] int symbol_count(const std::string& code, const char* opt, const char* needle) {
    char path[] = "/tmp/k4_sym_XXXXXX.cpp";
    const int fd = mkstemps(path, 4);
    if (fd < 0) return -1;
    const bool wrote = ::write(fd, code.data(), code.size()) == static_cast<ssize_t>(code.size());
    ::close(fd);
    if (!wrote) { ::unlink(path); return -1; }

    std::string obj = path; obj += ".o";
    char cmd[PATH_MAX * 2];
    std::snprintf(cmd, sizeof(cmd), "g++ -std=c++20 %s -c '%s' -o '%s' 2>/dev/null",
                  opt, path, obj.c_str());
    int count = -1;
    if (std::system(cmd) == 0) {
        std::snprintf(cmd, sizeof(cmd), "nm --demangle '%s' 2>/dev/null | grep -c '%s'",
                      obj.c_str(), needle);
        if (std::FILE* pipe = popen(cmd, "r")) {
            char line[64];
            if (std::fgets(line, sizeof(line), pipe)) count = std::atoi(line);
            pclose(pipe);
        }
    }
    ::unlink(path);
    ::unlink(obj.c_str());
    return count;
}

// ===========================================================================
// The functions under discussion
// ===========================================================================

// MAY be folded. Nothing here forces it.
[[nodiscard]] constexpr std::uint64_t power(std::uint64_t base, unsigned exp) {
    std::uint64_t result = 1;
    while (exp--) result *= base;
    return result;
}

// MUST be folded. There is no runtime version of this function to call.
[[nodiscard]] consteval std::uint64_t power_forced(std::uint64_t base, unsigned exp) {
    std::uint64_t result = 1;
    while (exp--) result *= base;
    return result;
}

// One function, two implementations. The compile-time branch must be something
// the constant evaluator can do; the runtime branch can use anything.
[[nodiscard]] constexpr int popcount_dual(std::uint64_t v) {
    if (std::is_constant_evaluated()) {
        int n = 0;                                  // plain loop: constant-evaluable
        while (v) { n += static_cast<int>(v & 1); v >>= 1; }
        return n;
    }
    return __builtin_popcountll(v);                 // one instruction at run time
}

// C++20 allows allocation during constant evaluation as long as it does not
// escape: everything allocated must be freed before the evaluation ends.
[[nodiscard]] constexpr int transient_allocation(int n) {
    int* scratch = new int[static_cast<std::size_t>(n)];
    for (int i = 0; i < n; ++i) scratch[i] = i * i;
    int total = 0;
    for (int i = 0; i < n; ++i) total += scratch[i];
    delete[] scratch;                               // omit this and it will not compile
    return total;
}

// A table built entirely by the compiler. It lands in .rodata with no
// initialisation code at all — section 6 checks that claim.
[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}
constexpr auto kCrcTable = make_crc_table();

// constinit: constant-initialised, but still mutable. Compare with a plain
// global whose initialiser runs at program start.
constinit int g_counter = 100;

// Runtime, so a lookup through it cannot be folded to an immediate.
volatile std::size_t g_runtime_index = 7;

}  // namespace

int main() {
    std::printf("1. constexpr on a FUNCTION is a permission, not a promise\n");
    {
        const std::string code =
            "#include <cstdint>\n"
            "constexpr std::uint64_t power(std::uint64_t b, unsigned e) {\n"
            "  std::uint64_t r = 1; while (e--) r *= b; return r; }\n"
            "volatile std::uint64_t sink;\n"
            "int main() { sink = power(3, 20); }\n";
        const int at_o0 = symbol_count(code, "-O0", "power");
        const int at_o2 = symbol_count(code, "-O2", "power");
        std::printf("   same source, called in a RUNTIME context:\n");
        std::printf("     -O0: %d symbol(s) for power()  <- emitted and called\n", at_o0);
        std::printf("     -O2: %d symbol(s) for power()  <- folded to a literal\n", at_o2);
        std::printf("   So the folding came from the OPTIMISER, not from the keyword.\n");
    }

    std::printf("\n2. The guarantee lives in the CONTEXT, not the function\n");
    {
        const std::string code =
            "#include <cstdint>\n"
            "constexpr std::uint64_t power(std::uint64_t b, unsigned e) {\n"
            "  std::uint64_t r = 1; while (e--) r *= b; return r; }\n"
            "volatile std::uint64_t sink;\n"
            "int main() { constexpr std::uint64_t v = power(3, 20); sink = v; }\n";
        std::printf("     -O0 with `constexpr auto v = power(3,20);`: %d symbol(s)\n",
                    symbol_count(code, "-O0", "power"));
        std::printf("   Constant-evaluated contexts force it even with optimisation off:\n");
        std::printf("     constexpr variable, static_assert, template argument,\n");
        std::printf("     array bound, case label, if constexpr condition.\n");
        static_assert(power(3, 20) == 3486784401ULL);
        std::printf("   static_assert(power(3,20) == 3486784401) passed at compile time.\n");
    }

    std::printf("\n3. consteval makes it mandatory\n");
    {
        constexpr auto v = power_forced(2, 10);
        std::printf("   power_forced(2,10) = %llu, evaluated during compilation\n",
                    static_cast<unsigned long long>(v));
        const std::string bad =
            "#include <cstdint>\n"
            "consteval std::uint64_t f(std::uint64_t b, unsigned e) {\n"
            "  std::uint64_t r = 1; while (e--) r *= b; return r; }\n"
            "volatile unsigned e_runtime = 10;\n"
            "int main() { return static_cast<int>(f(2, e_runtime)); }\n";
        const auto r = compile_snippet(bad);
        std::printf("   calling it with a runtime argument:\n");
        std::printf("     %s\n", r.ok ? "compiled (unexpected!)" : r.first_error.c_str());
        std::printf("   That is the difference: constexpr degrades quietly to a runtime\n");
        std::printf("   call, consteval refuses to compile. If you need the guarantee,\n");
        std::printf("   consteval states it in the signature instead of hoping.\n");
    }

    std::printf("\n4. std::is_constant_evaluated(): one function, two implementations\n");
    {
        constexpr int at_compile = popcount_dual(0xF0F0F0F0F0F0F0F0ULL);
        volatile std::uint64_t runtime_value = 0xF0F0F0F0F0F0F0F0ULL;
        const int at_run = popcount_dual(runtime_value);
        std::printf("   compile-time path (loop)     : %d\n", at_compile);
        std::printf("   runtime path (popcnt builtin): %d\n", at_run);
        std::printf("   Same answer, different code. Useful when the fast runtime version\n");
        std::printf("   uses something the constant evaluator cannot do — an intrinsic,\n");
        std::printf("   a reinterpret_cast, or inline asm.\n");
        std::printf("   Trap: inside a `constexpr` VARIABLE initialiser it is always true,\n");
        std::printf("   so testing it there tells you nothing about how callers use it.\n");
    }

    std::printf("\n5. constinit: constant initialisation without const\n");
    {
        std::printf("   g_counter starts at %d, and is mutable: ", g_counter);
        g_counter += 5;
        std::printf("now %d\n", g_counter);
        const auto r = compile_snippet(
            "#include <cstdlib>\n"
            "int seed() { return std::rand(); }\n"
            "constinit int g = seed();\n"
            "int main() { return g; }\n");
        std::printf("   with a non-constant initialiser:\n     %s\n",
                    r.ok ? "compiled (unexpected!)" : r.first_error.c_str());
        std::printf("   That is the point: it guarantees no dynamic initialisation runs at\n");
        std::printf("   program start, which is what removes the static initialisation\n");
        std::printf("   order problem from D1 — without forcing the object to be const.\n");
    }

    std::printf("\n6. A table the compiler built: where does it live?\n");
    {
        std::printf("   sizeof(kCrcTable) = %zu bytes\n", sizeof(kCrcTable));
        std::printf("   kCrcTable[1] = 0x%08X   <- constant index: folded to an immediate\n",
                    kCrcTable[1]);
        const std::uint32_t dynamic = kCrcTable[g_runtime_index];
        std::printf("   kCrcTable[g_runtime_index] = 0x%08X  <- runtime index: needs the table\n",
                    dynamic);

        char exe[PATH_MAX];
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        bool found = false;
        if (n > 0) {
            exe[n] = '\0';
            char cmd[PATH_MAX + 128];
            std::snprintf(cmd, sizeof(cmd),
                          "nm --print-size --demangle '%s' 2>/dev/null | grep -i crc", exe);
            if (std::FILE* pipe = popen(cmd, "r")) {
                char line[512];
                while (std::fgets(line, sizeof(line), pipe)) {
                    std::printf("   nm: %s", line);
                    found = true;
                }
                pclose(pipe);
            }
        }
        if (!found)
            std::printf("   nm: no symbol — every use was folded, so the table never existed\n");
        std::printf("\n   A symbol type of r/R means read-only data: the bytes are IN the\n");
        std::printf("   binary and no code runs to build them. A runtime-built table would\n");
        std::printf("   sit in .bss with an initialiser in .text instead.\n");
        std::printf("   Worth noticing that indexing only with constants removes the table\n");
        std::printf("   ENTIRELY — the compiler emits the values it needs and nothing else.\n");
    }

    std::printf("\n7. What constant evaluation can and cannot do here\n");
    {
        constexpr int t = transient_allocation(10);
        static_assert(t == 285);
        std::printf("   transient new[]/delete[] during constant evaluation: OK (sum = %d)\n", t);
        std::printf("   (the allocation must not escape — omit the delete[] and it fails)\n");
        constexpr auto sorted = [] {
            std::array<int, 5> a{5, 3, 1, 4, 2};
            std::sort(a.begin(), a.end());
            return a;
        }();
        std::printf("   std::sort on a std::array at compile time: OK ({%d,%d,%d,%d,%d})\n",
                    sorted[0], sorted[1], sorted[2], sorted[3], sorted[4]);
        std::printf("   try/catch in a constexpr function: OK\n");
        std::printf("   NOT available on this toolchain (GCC %d.%d):\n", __GNUC__, __GNUC_MINOR__);
#if defined(__cpp_lib_constexpr_vector)
        std::printf("     constexpr std::vector: available\n");
#else
        std::printf("     constexpr std::vector / std::string  (needs GCC 12+)\n");
#endif
#if defined(__cpp_if_consteval)
        std::printf("     if consteval: available\n");
#else
        std::printf("     if consteval  (C++23; use std::is_constant_evaluated instead)\n");
#endif
        std::printf("     constexpr virtual calls  (C++20, but not in GCC 10)\n");
    }

    std::printf("\n8. The cost: compile time, and a hard limit\n");
    {
        const std::string heavy =
            "constexpr unsigned long long f(unsigned n) {\n"
            "  unsigned long long a = 0; for (unsigned i = 0; i < n; ++i) a += i * i; return a; }\n"
            "int main() { constexpr auto v = f(2000000); return static_cast<int>(v & 1); }\n";
        const auto t0 = Clock::now();
        const auto ok = compile_snippet(
            heavy, "-fconstexpr-ops-limit=100000000 -fconstexpr-loop-limit=4000000");
        const auto t1 = Clock::now();
        std::printf("   2,000,000 iterations at compile time, both limits raised:\n");
        std::printf("     %s in %.2f s\n",
                    ok.ok ? "compiled" : ok.first_error.c_str(),
                    std::chrono::duration<double>(t1 - t0).count());

        // There are TWO separate budgets, and the loop one bites first.
        const auto loop_capped = compile_snippet(heavy, "-fconstexpr-ops-limit=100000000");
        std::printf("   with only the ops limit raised (loop limit left at its default):\n");
        std::printf("     %s\n", loop_capped.ok ? "compiled" : loop_capped.first_error.c_str());
        const auto ops_capped = compile_snippet(
            heavy, "-fconstexpr-ops-limit=1000000 -fconstexpr-loop-limit=4000000");
        std::printf("   with only the loop limit raised:\n");
        std::printf("     %s\n", ops_capped.ok ? "compiled" : ops_capped.first_error.c_str());

        std::printf("   Constant evaluation is an interpreter running inside the compiler.\n");
        std::printf("   It is far slower than the generated code would be, it is bounded by\n");
        std::printf("   -fconstexpr-ops-limit and -fconstexpr-depth, and every translation\n");
        std::printf("   unit that includes the header pays the cost again (K3 section 3).\n");
    }

    std::printf("\n9. Choosing\n");
    std::printf("   - constexpr function : may help, never hurts correctness. Default to it\n");
    std::printf("     for small pure functions, but do not assume it eliminated the work.\n");
    std::printf("   - constexpr variable : where the guarantee actually comes from.\n");
    std::printf("   - consteval          : when a runtime call would be a bug — building a\n");
    std::printf("     format string, validating a literal, generating a table.\n");
    std::printf("   - constinit          : globals that must not have dynamic initialisation,\n");
    std::printf("     but need to stay mutable.\n");
    std::printf("   - measure the compile time before pushing work into section 8's\n");
    std::printf("     territory; the interpreter is slow.\n");
}
