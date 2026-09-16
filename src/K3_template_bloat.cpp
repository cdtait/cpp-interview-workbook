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
    [[gnu::noinline]] std::size_t append(const T& value, std::uint64_t seed) {
        // Deliberately more than a one-liner, and deliberately independent of T
        // apart from sizeof(T). This is the code that gets duplicated.
        std::uint64_t h = seed;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            h ^= static_cast<std::uint64_t>(reinterpret_cast<const unsigned char*>(&value)[i]);
            h *= 0x100000001b3ULL;
            h ^= h >> 29;
            h += (h << 7) ^ (h >> 11);
        }
        used_ += sizeof(T);
        hash_ ^= h;
        return used_;
    }
    std::size_t   used_ = 0;
    std::uint64_t hash_ = 0;
};

// ===========================================================================
// The THIN version: the type-independent work moves into ONE non-template
// function, and the template becomes a type-safe wrapper thin enough to inline
// away entirely. Same interface, same safety, one copy of the logic.
// ===========================================================================
[[gnu::noinline]] std::size_t append_bytes(const void* data, std::size_t size,
                                           std::uint64_t seed,
                                           std::size_t* used, std::uint64_t* hash) {
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<const unsigned char*>(data)[i]);
        h *= 0x100000001b3ULL;
        h ^= h >> 29;
        h += (h << 7) ^ (h >> 11);
    }
    *used += size;
    *hash ^= h;
    return *used;
}

template <typename T>
struct ThinBuffer {
    std::size_t append(const T& value, std::uint64_t seed) {
        return append_bytes(&value, sizeof(T), seed, &used_, &hash_);
    }
    std::size_t   used_ = 0;
    std::uint64_t hash_ = 0;
};

// ===========================================================================
// Recursive pack expansion instantiates one function per pack length; a fold
// instantiates one function, full stop.
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

[[nodiscard]] SymStats symbols_matching(const char* needle) {
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

// Force every instantiation to be emitted, so nm can see it.
std::atomic<std::size_t> g_sink{0};

template <template <typename> class Buffer, int... Is>
void touch_all(std::integer_sequence<int, Is...>) {
    ((g_sink.fetch_add(Buffer<Tag<Is>>{}.append(Tag<Is>{}, 0x1234u),
                       std::memory_order_relaxed)), ...);
}

}  // namespace

int main() {
    std::printf("1. One template, sixteen types: what ends up in the binary\n");
    {
        touch_all<FatBuffer>(std::make_integer_sequence<int, 16>{});
        touch_all<ThinBuffer>(std::make_integer_sequence<int, 16>{});

        // nm qualifies template arguments, so a body is
        //   (anonymous namespace)::FatBuffer<(anonymous namespace)::Tag<0> >::append
        // while the driver is touch_all<(anonymous namespace)::FatBuffer, 0, 1, ...>.
        // The trailing '<' is what separates the two.
        const auto fat_bodies  = symbols_matching("FatBuffer<");
        const auto thin_bodies = symbols_matching("ThinBuffer<");
        const auto fat_all     = symbols_matching("FatBuffer");
        const auto thin_all    = symbols_matching("ThinBuffer");
        const auto shared      = symbols_matching("append_bytes");

        std::printf("   %-48s %8s %10s\n", "", "symbols", "code bytes");
        std::printf("   %-48s %8d %10ld\n", "FatBuffer<Tag<N>>::append  (one body per type)",
                    fat_bodies.count, fat_bodies.bytes);
        std::printf("   %-48s %8d %10ld\n", "ThinBuffer<Tag<N>>::append (one body per type)",
                    thin_bodies.count, thin_bodies.bytes);
        std::printf("   %-48s %8d %10ld\n", "append_bytes               (shared, non-template)",
                    shared.count, shared.bytes);
        std::printf("\n   ThinBuffer emitted %d out-of-line bodies: each wrapper is a single\n",
                    thin_bodies.count);
        std::printf("   call, so all sixteen inlined into the caller and vanished.\n\n");

        const long fat_total  = fat_all.bytes;
        const long thin_total = thin_all.bytes + shared.bytes;
        std::printf("   %-48s %8d %10ld\n", "FAT family total  (bodies + caller)",
                    fat_all.count, fat_total);
        std::printf("   %-48s %8d %10ld\n", "THIN family total (caller + shared body)",
                    thin_all.count + shared.count, thin_total);
        if (thin_total > 0 && fat_total > 0)
            std::printf("\n   %.1fx less code for identical behaviour and identical type safety.\n",
                        static_cast<double>(fat_total) / static_cast<double>(thin_total));
        std::printf("   The ratio grows with both the size of the duplicated body and the\n");
        std::printf("   number of types; sixteen small ones is a conservative case.\n");
        std::printf("   The wrappers are small enough to inline, so the type checking is\n");
        std::printf("   free and only the type-INDEPENDENT work is shared.\n");
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
