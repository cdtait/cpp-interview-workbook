// J1 — memory ordering: what you write, what the compiler keeps, what x86 does.
//
// Three separate things reorder your memory operations, and they are easy to
// conflate:
//
//   1. THE COMPILER, which may move, merge or delete accesses it can prove
//      nothing observes. std::atomic and `asm volatile ::: "memory"` stop it;
//      `volatile` alone stops only the moving and merging, not much else.
//   2. THE CPU, which on x86-64 is Total Store Order: loads are not reordered
//      with loads, stores not with stores, and a load is never moved after a
//      later store. The ONE reordering x86 allows is StoreLoad — an earlier
//      store becoming visible after a later load, because the store sits in a
//      per-core store buffer.
//   3. THE CACHE COHERENCE PROTOCOL, which is not a reordering source at all;
//      it is already coherent. This is the part people blame wrongly.
//
// Because of (2), most of the C++ memory orders cost NOTHING on x86: acquire and
// release are plain `mov`. Only seq_cst stores need a real barrier, because only
// StoreLoad has to be prevented. §1 shows this by disassembling this very
// binary at run time, and §2 proves the one remaining reordering is real by
// catching it happening.
//
// On ARM or POWER the same source compiles to very different instructions —
// acquire/release are not free there. Reading the x86 output and concluding
// "ordering is free" is a common and expensive mistake.

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <climits>
#include <random>
#include <semaphore.h>
#include <unistd.h>
#include <x86intrin.h>

namespace {
std::atomic<int> g_atomic{0};
std::atomic<int> g_atomic_b{0};
}  // namespace

// Deliberately NOT in the anonymous namespace. With internal linkage and no
// escaping address, GCC can prove an asm "memory" clobber cannot reach these,
// and deletes the stores entirely — which would quietly destroy the two demos
// below. External linkage is what makes the barrier observable.
int          g_plain = 0;
volatile int g_volatile = 0;

// Each primitive is its own non-inlined, unmangled function so it can be found
// in the symbol table and disassembled below.
extern "C" {

[[gnu::noinline]] void store_relaxed(int v) { g_atomic.store(v, std::memory_order_relaxed); }
[[gnu::noinline]] void store_release(int v) { g_atomic.store(v, std::memory_order_release); }
[[gnu::noinline]] void store_seq_cst(int v) { g_atomic.store(v, std::memory_order_seq_cst); }

[[gnu::noinline]] int load_relaxed()  { return g_atomic.load(std::memory_order_relaxed); }
[[gnu::noinline]] int load_acquire()  { return g_atomic.load(std::memory_order_acquire); }
[[gnu::noinline]] int load_seq_cst()  { return g_atomic.load(std::memory_order_seq_cst); }

[[gnu::noinline]] int rmw_relaxed()   { return g_atomic.fetch_add(1, std::memory_order_relaxed); }
[[gnu::noinline]] int rmw_seq_cst()   { return g_atomic.fetch_add(1, std::memory_order_seq_cst); }

[[gnu::noinline]] bool cas_acq_rel(int expected, int desired) {
    return g_atomic.compare_exchange_strong(expected, desired,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire);
}

[[gnu::noinline]] void fence_acquire() { std::atomic_thread_fence(std::memory_order_acquire); }
[[gnu::noinline]] void fence_release() { std::atomic_thread_fence(std::memory_order_release); }
[[gnu::noinline]] void fence_seq_cst() { std::atomic_thread_fence(std::memory_order_seq_cst); }

// Two ordinary stores to the same variable: the compiler is free to drop the
// first one entirely, because nothing can observe it.
[[gnu::noinline]] void plain_two_stores() { g_plain = 1; g_plain = 2; }

// volatile forbids merging and reordering *of volatile accesses by the
// compiler*. It emits no barrier and makes no promise to other threads.
[[gnu::noinline]] void volatile_two_stores() { g_volatile = 1; g_volatile = 2; }

// A compiler barrier: no instruction is emitted, but the compiler may not move
// memory accesses across it. This is ordering (1) without ordering (2).
[[gnu::noinline]] void barrier_two_stores() {
    g_plain = 1;
    asm volatile("" ::: "memory");
    g_plain = 2;
}

// The classic trap: volatile is not atomic. This is a separate load, add and
// store, with no lock prefix, so two threads can lose an increment.
[[gnu::noinline]] void volatile_increment() { g_volatile = g_volatile + 1; }

// Release store followed by an unrelated load: the StoreLoad pair that x86 is
// allowed to reorder, and the reason §2 works.
[[gnu::noinline]] int store_then_load() {
    g_atomic.store(1, std::memory_order_release);
    return g_atomic_b.load(std::memory_order_acquire);
}

// The same pair with seq_cst, which is the only variant that costs an
// instruction on this architecture.
[[gnu::noinline]] int store_then_load_seq_cst() {
    g_atomic.store(1, std::memory_order_seq_cst);
    return g_atomic_b.load(std::memory_order_seq_cst);
}

}  // extern "C"

namespace {

// Disassembles one function out of this running binary.
void show(const char* symbol, const char* note) {
    // NOT /proc/self/exe in the command: inside the popen'd shell that resolves
    // to objdump's own binary. Resolve it here, in this process, first.
    char exe[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { std::printf("   %-24s (cannot resolve own path)\n", symbol); return; }
    exe[n] = '\0';

    char cmd[PATH_MAX + 256];
    std::snprintf(cmd, sizeof(cmd),
                  "objdump -d --disassemble=%s --no-show-raw-insn '%s' 2>/dev/null",
                  symbol, exe);
    std::FILE* pipe = popen(cmd, "r");
    if (!pipe) { std::printf("   %-26s (objdump unavailable)\n", symbol); return; }

    std::printf("   %-24s %s\n", symbol, note);
    char line[512];
    bool in_body = false;
    while (std::fgets(line, sizeof(line), pipe)) {
        if (std::strstr(line, ">:")) { in_body = true; continue; }
        if (!in_body) continue;
        if (line[0] == '\n') break;
        // strip the address column and any trailing comment
        char* tab = std::strchr(line, '\t');
        const char* insn = tab ? tab + 1 : line;
        if (std::strstr(insn, "nop")) continue;              // alignment padding
        char* hash = std::strchr(const_cast<char*>(insn), '#');
        if (hash) *hash = '\0';
        std::printf("        %s\n", insn);
    }
    pclose(pipe);
}

// ---------------------------------------------------------------------------
// §2 — catching StoreLoad reordering in the act.
//
//   thread A:  x = 1;  r1 = y;
//   thread B:  y = 1;  r2 = x;
//
// If neither store may be delayed past the following load, at least one thread
// must see the other's 1, so r1 == 0 && r2 == 0 is impossible. On x86 the stores
// sit in a store buffer, so both threads can read 0. That outcome IS the
// reordering, directly observed.
// ---------------------------------------------------------------------------
struct Litmus {
    std::atomic<int> x{0}, y{0};
    std::atomic<int> r1{0}, r2{0};
    sem_t begin_a{}, begin_b{}, done{};
};

// A tight barrier is the wrong tool here: it lines the threads up so precisely
// that the store buffer has drained by the time the other side loads, and the
// outcome is never seen. Semaphores plus a RANDOM delay on each side decorrelate
// the pair, which is what makes the window occasionally line up.
// The memory order MUST be a template parameter. Passing std::memory_order as a
// runtime value makes the compiler emit the conservative general case, so every
// configuration behaves like seq_cst and the experiment silently measures
// nothing. That bug produced three identical rows of zero before it was found.
template <int Which, std::memory_order StoreOrder, std::memory_order LoadOrder>
void litmus_side(Litmus* v, std::size_t rounds, unsigned seed) {
    // A tight barrier is the wrong tool: it lines the threads up so precisely
    // that the store buffer has drained before the other side loads. Semaphores
    // plus a RANDOM delay decorrelate the pair so the window sometimes lines up.
    // The delay must be slow enough to matter — a 3-cycle xorshift gives ~0.01%,
    // mt19937 about 0.2%.
    std::mt19937 rng{seed};
    for (std::size_t i = 0; i < rounds; ++i) {
        sem_wait(Which == 0 ? &v->begin_a : &v->begin_b);
        while (rng() % 64 != 0) {}
        if constexpr (Which == 0) {
            v->x.store(1, StoreOrder);
            v->r1.store(v->y.load(LoadOrder), std::memory_order_relaxed);
        } else {
            v->y.store(1, StoreOrder);
            v->r2.store(v->x.load(LoadOrder), std::memory_order_relaxed);
        }
        sem_post(&v->done);
    }
}

template <std::memory_order StoreOrder, std::memory_order LoadOrder>
[[nodiscard]] std::size_t run_litmus(std::size_t rounds) {
    Litmus v;
    sem_init(&v.begin_a, 0, 0);
    sem_init(&v.begin_b, 0, 0);
    sem_init(&v.done, 0, 0);

    std::thread ta(litmus_side<0, StoreOrder, LoadOrder>, &v, rounds, 12345u);
    std::thread tb(litmus_side<1, StoreOrder, LoadOrder>, &v, rounds, 67890u);

    std::size_t both_zero = 0;
    for (std::size_t i = 0; i < rounds; ++i) {
        v.x.store(0, std::memory_order_relaxed);
        v.y.store(0, std::memory_order_relaxed);
        sem_post(&v.begin_a);
        sem_post(&v.begin_b);
        sem_wait(&v.done);
        sem_wait(&v.done);
        if (v.r1.load(std::memory_order_relaxed) == 0 &&
            v.r2.load(std::memory_order_relaxed) == 0) ++both_zero;
    }
    ta.join();
    tb.join();
    sem_destroy(&v.begin_a);
    sem_destroy(&v.begin_b);
    sem_destroy(&v.done);
    return both_zero;
}

}  // namespace

int main() {
    std::printf("1. What each ordering actually compiles to on x86-64\n");
    std::printf("   (disassembled from this running binary, -O2)\n\n");

    std::printf("   STORES\n");
    show("store_relaxed", "// relaxed");
    show("store_release", "// release  <- identical to relaxed");
    show("store_seq_cst", "// seq_cst  <- the only store that costs anything");

    std::printf("\n   LOADS\n");
    show("load_relaxed", "// relaxed");
    show("load_acquire", "// acquire  <- identical to relaxed");
    show("load_seq_cst", "// seq_cst  <- also identical; loads are already ordered");

    std::printf("\n   READ-MODIFY-WRITE (always a locked instruction, any ordering)\n");
    show("rmw_relaxed", "// fetch_add relaxed");
    show("rmw_seq_cst", "// fetch_add seq_cst  <- same instruction");
    show("cas_acq_rel", "// compare_exchange_strong");

    std::printf("\n   FENCES\n");
    show("fence_acquire", "// acquire fence: compiler-only, no instruction");
    show("fence_release", "// release fence: compiler-only, no instruction");
    show("fence_seq_cst", "// seq_cst fence: a real mfence");

    std::printf("\n   COMPILER ORDERING, WITHOUT ANY CPU BARRIER\n");
    show("plain_two_stores", "// two plain stores: the first is deleted outright");
    show("volatile_two_stores", "// volatile: both stores survive, in order, no barrier");
    show("barrier_two_stores", "// asm memory clobber: both survive, zero instructions added");
    show("volatile_increment", "// volatile is NOT atomic: load, add, store, no lock prefix");

    std::printf("\n   THE ONE PAIR x86 MAY REORDER\n");
    show("store_then_load", "// release store then acquire load: nothing separates them");
    show("store_then_load_seq_cst", "// seq_cst: note the barrier that appears");

    std::printf("\n2. Catching the StoreLoad reordering happen\n");
    std::printf("   thread A: x=1 then read y     thread B: y=1 then read x\n");
    std::printf("   If stores could not be delayed, r1==0 && r2==0 would be impossible.\n\n");
    constexpr std::size_t kRounds = 200000;
    std::printf("   %-20s %14s %12s\n", "ordering", "both read 0", "rate");
    {
        const std::size_t a = run_litmus<std::memory_order_relaxed,
                                         std::memory_order_relaxed>(kRounds);
        const std::size_t b = run_litmus<std::memory_order_release,
                                         std::memory_order_acquire>(kRounds);
        const std::size_t c = run_litmus<std::memory_order_seq_cst,
                                         std::memory_order_seq_cst>(kRounds);
        const char* names[] = {"relaxed / relaxed", "release / acquire", "seq_cst / seq_cst"};
        const std::size_t counts[] = {a, b, c};
        for (int i = 0; i < 3; ++i)
            std::printf("   %-20s %14zu %11.4f%%\n", names[i], counts[i],
                        100.0 * static_cast<double>(counts[i]) / static_cast<double>(kRounds));
    }
    std::printf("\n   Non-zero counts are the store buffer, observed directly. seq_cst\n");
    std::printf("   should be exactly 0 — that is what its extra instruction buys, and\n");
    std::printf("   the only thing it buys on this architecture.\n");
}
