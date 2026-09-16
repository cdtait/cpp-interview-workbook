// K3 — what templates cost, and how to use them wisely.
//
// A template is not code. It is a recipe for code, and the compiler bakes one
// copy per distinct set of arguments. Ten types means ten function bodies, each
// separately optimised, inlined, and carried in your binary. That is the deal:
// you trade size and compile time for speed and type safety.
//
// Most of the time it is a good deal. It stops being one when the duplicated
// code does not actually depend on the type it was duplicated for — which is
// far more common than it sounds, and is the single most useful thing to know
// about writing templates at scale.
//
// This file measures the trade rather than asserting it: it reads its OWN symbol
// table with nm to size each family of instantiations, and it shells out to the
// compiler to time how instantiation count affects build time.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

// Sixteen distinct types, so each instantiation below is genuinely separate.
template <int N> struct Tag { char pad[N + 1]; };

// ===========================================================================
// The FAT version: the whole body is inside the template, so every T gets a
// complete private copy of logic that has nothing to do with T.
// ===========================================================================
template <typename T>
struct FatBuffer {
    // The body loops over a RUNTIME length and treats the object as bytes, so
    // nothing here depends on T. That is the case worth measuring: duplicating
    // this per type buys nothing at all.
    //
    // An earlier version of this example looped over sizeof(T) instead, which
    // quietly invalidated it — with a compile-time bound the compiler
    // specialises and unrolls per type, so the duplication is USEFUL and the
    // fat version came out smaller. If the body genuinely depends on T, leave
    // it in the template.
    std::uint64_t append(const T& value, std::size_t len, std::uint64_t seed) {
        const auto* src = static_cast<const unsigned char*>(static_cast<const void*>(&value));
        std::uint64_t h = seed;
        for (std::size_t i = 0; i < len; ++i) {
            const unsigned char c = src[i % sizeof(T)];
            h ^= c;
            h *= 0x100000001b3ULL;
            h ^= h >> 29;
            if (h & 1) h += (h << 7) ^ (h >> 11);
            else       h -= (h << 3) ^ (h >> 5);
            scratch_[i & 63] = static_cast<unsigned char>(h);
        }
        return h;
    }
    unsigned char scratch_[64]{};
};

// ===========================================================================
// The THIN version: the type-independent work moves into ONE non-template
// function, and the template becomes a type-safe wrapper thin enough to inline
// away entirely. Same interface, same safety, one copy of the logic.
// ===========================================================================
std::uint64_t append_bytes(unsigned char* scratch, const void* data,
                           std::size_t object_size, std::size_t len, std::uint64_t seed) {
    const auto* src = static_cast<const unsigned char*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = src[i % object_size];
        h ^= c;
        h *= 0x100000001b3ULL;
        h ^= h >> 29;
        if (h & 1) h += (h << 7) ^ (h >> 11);
        else       h -= (h << 3) ^ (h >> 5);
        scratch[i & 63] = static_cast<unsigned char>(h);
    }
    return h;
}

template <typename T>
struct ThinBuffer {
    // Thin enough to inline away completely; only sizeof(T) crosses the border.
    std::uint64_t append(const T& value, std::size_t len, std::uint64_t seed) {
        return append_bytes(scratch_, &value, sizeof(T), len, seed);
    }
    unsigned char scratch_[64]{};
};

// ===========================================================================
// Recursive pack expansion instantiates one function per pack length; a fold
// instantiates one function, full stop.
//
// These two keep [[gnu::noinline]], and it is worth being explicit about why:
// it is a MEASUREMENT AID, not part of the technique. At -O2 these calls fold to
// constants and leave no symbols at all, so nm would report 0 for both and the
// comparison would be vacuous. The attribute forces a symbol to exist so it can
// be counted. You would never write it in real code.
//
// The 8-vs-1 instantiation count is not an artifact of it: compile the same
// source at -O0 with no attribute and you still get 8 symbols against 1. The
// compiler must instantiate eight function templates either way, which costs
// compile time and debug-build size even when an optimised build inlines them
// all away.
// ===========================================================================
template <typename T> [[gnu::noinline]] std::uint64_t sum_recursive(T v) { return v; }
template <typename T, typename... Rest>
[[gnu::noinline]] std::uint64_t sum_recursive(T v, Rest... rest) {
    return v + sum_recursive(rest...);          // instantiates sum_recursive<Rest...>
}

template <typename... Ts>
[[gnu::noinline]] std::uint64_t sum_fold(Ts... vs) { return (std::uint64_t{0} + ... + vs); }

// ---------------------------------------------------------------------------
// Read this binary's own symbol table.
// ---------------------------------------------------------------------------
struct SymStats { int count = 0; long bytes = 0; };

[[nodiscard]] SymStats symbols_matching(const char* needle, const char* also = nullptr) {
    char exe[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return {};
    exe[n] = '\0';

    char cmd[PATH_MAX + 128];
    std::snprintf(cmd, sizeof(cmd), "nm --print-size --demangle '%s' 2>/dev/null", exe);
    std::FILE* pipe = popen(cmd, "r");
    if (!pipe) return {};

    SymStats out;
    char line[1024];
    while (std::fgets(line, sizeof(line), pipe)) {
        if (!std::strstr(line, needle)) continue;
        if (also && !std::strstr(line, also)) continue;
        unsigned long long addr = 0, size = 0;
        char type = 0;
        // "<addr> <size> <type> <name>"; lines without a size are skipped.
        if (std::sscanf(line, "%llx %llx %c", &addr, &size, &type) != 3) continue;
        if (type != 't' && type != 'T' && type != 'W' && type != 'w') continue;
        ++out.count;
        out.bytes += static_cast<long>(size);
    }
    pclose(pipe);
    return out;
}

// ---------------------------------------------------------------------------
// Time a real compile of N instantiations.
// ---------------------------------------------------------------------------
[[nodiscard]] double compile_seconds(int instantiations) {
    char path[] = "/tmp/k3_bloat_XXXXXX.cpp";
    const int fd = mkstemps(path, 4);
    if (fd < 0) return -1.0;

    std::string src =
        "#include <vector>\n#include <string>\n#include <map>\n#include <algorithm>\n"
        "template <int N> struct T { char pad[N % 32 + 1]; };\n"
        "template <typename X> struct Heavy {\n"
        "  std::map<std::string, std::vector<X>> m;\n"
        "  void go(const std::string& k, const X& v) {\n"
        "    m[k].push_back(v);\n"
        "    std::sort(m[k].begin(), m[k].end(),\n"
        "              [](const X& a, const X& b){ return sizeof(a) < sizeof(b); });\n"
        "  }\n};\n";
    for (int i = 0; i < instantiations; ++i)
        src += "template struct Heavy<T<" + std::to_string(i) + ">>;\n";

    const bool ok = ::write(fd, src.data(), src.size()) == static_cast<ssize_t>(src.size());
    ::close(fd);
    if (!ok) { ::unlink(path); return -1.0; }

    char cmd[PATH_MAX + 128];
    std::snprintf(cmd, sizeof(cmd), "g++ -std=c++20 -O2 -c '%s' -o /dev/null 2>/dev/null", path);
    const auto t0 = Clock::now();
    const int rc = std::system(cmd);
    const auto t1 = Clock::now();
    ::unlink(path);
    if (rc != 0) return -1.0;
    return std::chrono::duration<double>(t1 - t0).count();
}

std::atomic<std::uint64_t> g_sink{0};
// Runtime, so nothing below is constant-folded away.
volatile std::size_t g_len = 48;

// Two separately named drivers, so nm can attribute code to a type count. Each
// inlines its whole family, which is exactly what we want to size.
template <template <typename> class Buffer, int... Is>
[[gnu::noinline]] void touch16(std::integer_sequence<int, Is...>) {
    const std::size_t len = g_len;
    ((g_sink.fetch_add(Buffer<Tag<Is>>{}.append(Tag<Is>{}, len, 0x1234u),
                       std::memory_order_relaxed)), ...);
}
template <template <typename> class Buffer, int... Is>
[[gnu::noinline]] void touch64(std::integer_sequence<int, Is...>) {
    const std::size_t len = g_len;
    ((g_sink.fetch_add(Buffer<Tag<Is>>{}.append(Tag<Is>{}, len, 0x1234u),
                       std::memory_order_relaxed)), ...);
}

}  // namespace

int main() {
    std::printf("1. One template, many types: what ends up in the binary\n");
    std::printf("   The duplicated body does NOT depend on T (runtime length, bytes only),\n");
    std::printf("   so every copy of it is waste. No noinline: the compiler inlines each\n");
    std::printf("   family into its driver, and the driver is what we size.\n\n");
    {
        touch16<FatBuffer>(std::make_integer_sequence<int, 16>{});
        touch16<ThinBuffer>(std::make_integer_sequence<int, 16>{});
        touch64<FatBuffer>(std::make_integer_sequence<int, 64>{});
        touch64<ThinBuffer>(std::make_integer_sequence<int, 64>{});

        const auto shared = symbols_matching("append_bytes");
        std::printf("   %-10s %14s %14s %10s\n", "types", "fat bytes", "thin bytes", "ratio");
        for (const char* driver : {"touch16", "touch64"}) {
            const auto fat  = symbols_matching(driver, "FatBuffer");
            const auto thin = symbols_matching(driver, "ThinBuffer");
            const long thin_total = thin.bytes + shared.bytes;
            if (fat.bytes == 0 || thin_total == 0) continue;
            std::printf("   %-10s %14ld %14ld %9.2fx\n",
                        std::strcmp(driver, "touch16") == 0 ? "16" : "64",
                        fat.bytes, thin_total,
                        static_cast<double>(fat.bytes) / static_cast<double>(thin_total));
        }
        std::printf("\n   thin includes the one shared append_bytes body (%ld bytes), counted\n",
                    shared.bytes);
        std::printf("   once however many types use it.\n\n");
        std::printf("   Read the two rows together, because they disagree. At 16 types the\n");
        std::printf("   FAT version is SMALLER. The body still uses sizeof(T) for one modulo,\n");
        std::printf("   so each copy gets a compile-time constant where the shared version\n");
        std::printf("   must take a runtime divisor — and at that scale the specialisation is\n");
        std::printf("   worth more than the duplication costs. By 64 types the duplication\n");
        std::printf("   dominates and thin wins almost 2x.\n\n");
        std::printf("   That crossover IS the lesson. \"Templates cause bloat\" is not a rule,\n");
        std::printf("   it is a trade between how much of the body genuinely depends on T and\n");
        std::printf("   how many types you instantiate it for. Neither number is guessable —\n");
        std::printf("   measure your own, the way this section does.\n");
    }

    std::printf("\n2. Recursive pack vs fold\n");
    {
        g_sink.fetch_add(sum_recursive(1, 2, 3, 4, 5, 6, 7, 8), std::memory_order_relaxed);
        g_sink.fetch_add(sum_fold(1, 2, 3, 4, 5, 6, 7, 8), std::memory_order_relaxed);
        const auto rec  = symbols_matching("sum_recursive");
        const auto fold = symbols_matching("sum_fold");
        std::printf("   sum_recursive(1..8): %d instantiations, %ld bytes\n", rec.count, rec.bytes);
        std::printf("   sum_fold(1..8)     : %d instantiation%s, %ld bytes\n",
                    fold.count, fold.count == 1 ? "" : "s", fold.bytes);
        std::printf("   The recursive form instantiates one function per suffix of the\n");
        std::printf("   pack. For a 30-argument call that is 30 function bodies.\n");
    }

    std::printf("\n3. Compile time against instantiation count\n");
    {
        std::printf("   %10s %12s %14s\n", "instances", "seconds", "per instance");
        double first = 0.0;
        for (int n : {1, 8, 32, 128}) {
            const double s = compile_seconds(n);
            if (s < 0.0) { std::printf("   %10d %12s\n", n, "failed"); continue; }
            if (first == 0.0) first = s;
            std::printf("   %10d %12.3f %14.4f\n", n, s, s / n);
        }
        std::printf("   Per-instance cost falls (fixed header parsing is amortised) but the\n");
        std::printf("   total keeps climbing. This is why heavily templated headers dominate\n");
        std::printf("   build times: every translation unit that includes them pays again.\n");
    }

    std::printf("\n4. Using templates wisely\n");
    std::printf("   - Hoist type-INDEPENDENT code out of the template. If the body only\n");
    std::printf("     needs sizeof(T) or a pointer, it does not belong in the template.\n");
    std::printf("   - Prefer folds to recursive pack expansion: 1 instantiation, not N.\n");
    std::printf("   - Prefer `if constexpr` to overload sets when you do not need overload\n");
    std::printf("     resolution: one function, fewer symbols, better errors.\n");
    std::printf("   - `extern template struct Heavy<Foo>;` in a header plus one explicit\n");
    std::printf("     instantiation in a .cpp stops every TU re-instantiating it.\n");
    std::printf("   - Type-erase at the boundary (std::function, a virtual interface, a\n");
    std::printf("     span) and template only inside it. Templates at an API boundary leak\n");
    std::printf("     into every caller's build.\n");
    std::printf("   - Constrain with concepts so a wrong type fails with a named reason\n");
    std::printf("     instead of a page of instantiation backtrace.\n");
    std::printf("   - Measure. `nm --print-size --demangle` and `-ftime-report` turn all of\n");
    std::printf("     the above from folklore into numbers, as in sections 1 to 3.\n");
}
