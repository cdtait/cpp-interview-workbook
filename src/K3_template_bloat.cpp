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
        std::size_t j = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const unsigned char c = src[j];
            if (++j == sizeof(T)) j = 0;        // identical to the shared version
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
// The workload both versions run. It is synthetic, and every part of it is
// chosen for a measurement reason rather than a realistic one:
//
//   len          a RUNTIME length, never sizeof(T). This is what makes the body
//                type-independent, which is the whole premise: with a
//                compile-time bound the compiler specialises and unrolls per
//                type, the duplication becomes USEFUL, and the fat version
//                legitimately wins.
//   the mixing   an FNV-style multiply plus xor-shifts. Each step depends on the
//                previous one, so the loop cannot be vectorised or reassociated
//                away.
//   the branch   a data-dependent if/else, so the body keeps a real basic-block
//                structure and a non-trivial size. Without it the loop collapses
//                and there is not enough code for duplication to show up in.
//   the store    a side effect the optimiser may not discard; `& 63` keeps it
//                inside the scratch buffer.
//   j, not i%n   the cyclic index advances by hand. An earlier version wrote
//                `src[i % object_size]`, which put a hardware `div` in the inner
//                loop of THIS function only: object_size is a runtime value
//                here, but sizeof(T) is a constant inside the template, so every
//                fat copy dodged a division the shared body paid on every byte.
//                That penalty has nothing to do with sharing code, and it was
//                large enough to reverse the 16-type result.
// [[maybe_unused]]: this definition is the readable reference. The measurement
// in section 1 compiles a generated copy of the same code, for the attribution
// reasons explained above generate_source().
[[maybe_unused]] std::uint64_t append_bytes(unsigned char* scratch, const void* data,
                                            std::size_t object_size, std::size_t len,
                                            std::uint64_t seed) {
    const auto* src = static_cast<const unsigned char*>(data);
    std::uint64_t h = seed;
    std::size_t j = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = src[j];
        if (++j == object_size) j = 0;          // same cost in both versions
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
// Measuring section 1 by reading THIS binary's symbols does not work, and it is
// worth saying why: the 16-type and 64-type drivers share instantiations
// (FatBuffer<Tag<0>> serves both), and the compiler inlines some bodies into
// their driver while leaving others out of line. Attribution becomes guesswork,
// and an early version of this file silently counted the fat family's driver
// while omitting 2757 bytes of out-of-line bodies — which reversed the result.
//
// So instead: generate a self-contained program per configuration, compile it,
// and read the .text size. Same approach the compile-time section already uses.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string generate_source(int types, bool thin) {
    std::string src =
        "#include <cstdint>\n#include <cstddef>\n#include <cstdlib>\n#include <utility>\n"
        "template <int N> struct Tag { char pad[N + 1]; };\n";
    if (thin) {
        src +=
            "std::uint64_t append_bytes(unsigned char* scratch, const void* data,\n"
            "                           std::size_t object_size, std::size_t len,\n"
            "                           std::uint64_t seed) {\n"
            "  const auto* src = static_cast<const unsigned char*>(data);\n"
            "  std::uint64_t h = seed; std::size_t j = 0;\n"
            "  for (std::size_t i = 0; i < len; ++i) {\n"
            "    const unsigned char c = src[j];\n"
            "    if (++j == object_size) j = 0;\n"
            "    h ^= c; h *= 0x100000001b3ULL; h ^= h >> 29;\n"
            "    if (h & 1) h += (h << 7) ^ (h >> 11); else h -= (h << 3) ^ (h >> 5);\n"
            "    scratch[i & 63] = static_cast<unsigned char>(h);\n"
            "  }\n  return h;\n}\n"
            "template <typename T> struct Buf {\n"
            "  std::uint64_t append(const T& v, std::size_t len, std::uint64_t s) {\n"
            "    return append_bytes(scratch_, &v, sizeof(T), len, s); }\n"
            "  unsigned char scratch_[64]{};\n};\n";
    } else {
        src +=
            "template <typename T> struct Buf {\n"
            "  std::uint64_t append(const T& v, std::size_t len, std::uint64_t seed) {\n"
            "    const auto* src = static_cast<const unsigned char*>(\n"
            "        static_cast<const void*>(&v));\n"
            "    std::uint64_t h = seed; std::size_t j = 0;\n"
            "    for (std::size_t i = 0; i < len; ++i) {\n"
            "      const unsigned char c = src[j];\n"
            "      if (++j == sizeof(T)) j = 0;\n"
            "      h ^= c; h *= 0x100000001b3ULL; h ^= h >> 29;\n"
            "      if (h & 1) h += (h << 7) ^ (h >> 11); else h -= (h << 3) ^ (h >> 5);\n"
            "      scratch_[i & 63] = static_cast<unsigned char>(h);\n"
            "    }\n    return h; }\n"
            "  unsigned char scratch_[64]{};\n};\n";
    }
    src += "volatile std::uint64_t sink;\n"
           "template <int... Is> void touch(std::uint64_t seed, std::size_t len,\n"
           "                                std::integer_sequence<int, Is...>) {\n"
           "  ((sink = sink + Buf<Tag<Is>>{}.append(Tag<Is>{}, len, seed + Is)), ...);\n}\n"
           "int main(int argc, char** argv) {\n"
           "  const std::uint64_t seed = argc > 1 ? std::atoll(argv[1]) : 7;\n"
           "  const std::size_t len = argc > 2 ? std::atoll(argv[2]) : 48;\n"
           "  touch(seed, len, std::make_integer_sequence<int, " + std::to_string(types) + ">{});\n}\n";
    return src;
}

// Compile the generated program and report its .text size in bytes.
[[nodiscard]] long text_bytes(int types, bool thin) {
    char src_path[] = "/tmp/k3_size_XXXXXX.cpp";
    const int fd = mkstemps(src_path, 4);
    if (fd < 0) return -1;
    const std::string src = generate_source(types, thin);
    const bool wrote = ::write(fd, src.data(), src.size()) == static_cast<ssize_t>(src.size());
    ::close(fd);
    if (!wrote) { ::unlink(src_path); return -1; }

    std::string bin = src_path;
    bin += ".bin";
    char cmd[PATH_MAX * 3];
    std::snprintf(cmd, sizeof(cmd), "g++ -std=c++20 -O2 '%s' -o '%s' 2>/dev/null",
                  src_path, bin.c_str());
    long bytes = -1;
    if (std::system(cmd) == 0) {
        std::snprintf(cmd, sizeof(cmd),
                      "size -A '%s' 2>/dev/null | awk '/^\\.text/{print $2}'", bin.c_str());
        if (std::FILE* pipe = popen(cmd, "r")) {
            char line[64];
            if (std::fgets(line, sizeof(line), pipe)) bytes = std::strtol(line, nullptr, 10);
            pclose(pipe);
        }
    }
    ::unlink(src_path);
    ::unlink(bin.c_str());
    return bytes;
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
    std::printf("   Identical bodies; the only difference is whether the type-independent\n");
    std::printf("   work lives inside the template or in one shared function. Each row is a\n");
    std::printf("   separately generated and compiled program, measured with size(1).\n\n");
    {
        std::printf("   %-8s %14s %14s %10s\n", "types", "fat .text", "thin .text", "ratio");
        for (int types : {4, 16, 64, 128}) {
            const long fat  = text_bytes(types, false);
            const long thin = text_bytes(types, true);
            if (fat < 0 || thin < 0) {
                std::printf("   %-8d %14s\n", types, "compile failed");
                continue;
            }
            std::printf("   %-8d %14ld %14ld %9.2fx\n", types, fat, thin,
                        static_cast<double>(fat) / static_cast<double>(thin));
        }
        std::printf("\n   The fat column grows with the type count because each type gets its\n");
        std::printf("   own copy of a body that never needed one. The thin column grows far\n");
        std::printf("   more slowly: only the wrapper is duplicated, and the wrapper inlines\n");
        std::printf("   to a call. The shared body is paid for exactly once.\n");
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
