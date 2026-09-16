# C++ Interview Workbook — VS Code Project

This project turns the 16 worksheet problems into runnable C++20 examples.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

Run any individual example, for example:

```bash
./build/A1_buffer_rule_of_zero
./build/B1_vector_move_if_noexcept
./build/C2_remove_erase
```

On Windows with a multi-config generator, executables may be under `build/Debug/`.

## Sanitizers

For the UB demonstrations, configure a separate sanitizer build with `-DSANITIZE`:

```bash
cmake -S . -B build-asan -DSANITIZE=address
cmake --build build-asan -j
./build-asan/A1_buffer_rule_of_zero
```

`SANITIZE=address` turns on AddressSanitizer *and* UndefinedBehaviorSanitizer, plus
`-fno-sanitize-recover=all` so UBSan aborts on the first finding instead of only printing.
Frame pointers and `-g` are added too, so reports carry readable stack traces.

ThreadSanitizer cannot be combined with ASan, so it gets its own directory:

```bash
cmake -S . -B build-tsan -DSANITIZE=thread
cmake --build build-tsan -j
```

Keep the plain `build/` directory around: comparing a clean build against a sanitizer
build is the point of problem E1. In VS Code the same builds are available as the
**CMake: build all (asan+ubsan)** task, and a single open file can be built with
**C/C++: g++ build active file (asan+ubsan)**.

Useful at run time:

```bash
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 ./build-asan/A4_shared_ptr_cycle
UBSAN_OPTIONS=print_stacktrace=1 ./build-asan/C4_container_surprises
```

The deliberately-dangerous lines are commented out by default so the project runs cleanly —
uncomment one, rebuild in `build-asan`, and the sanitizer should name the exact line.

## Problems

### A — Ownership and raw memory
- A1 `Buffer`: shallow copy, double free, suppressed implicit move, Rule of Zero
- A2 `delete`: nullptr, new[]/delete mismatch, polymorphic delete, delete nullptr
- A3 `make_shared`: ownership gap in pre-C++17 argument evaluation
- A4 `shared_ptr` cycle: use counts and `weak_ptr`

### B — Value semantics
- B1 `vector` reallocation and `noexcept`
- B2 return value optimisation and pessimizing `std::move`
- B3 what `std::move` actually does
- B4 parameter-passing choices for strings

### C — Containers and iterators
- C1 iterator invalidation during erase
- C2 `std::remove` versus actual container erasure
- C3 `unordered_map` rehash: stable references, invalid iterators
- C4 map `operator[]`, unsigned underflow, `vector<bool>` proxy

### D — Classes and initialisation
- D1 member initialisation order
- D2 object slicing
- D3 C++20 concepts

### E — Tooling
- E1 production-only bug investigation checklist
- E2 benchmarking methodology

### F — Putting it together
- F1 feed handling: four subscriber-notification designs, benchmarked and
  safety-tested against hostile callbacks — see [docs/F1_feed_handling.md](docs/F1_feed_handling.md)
- F2 a combined walkthrough of the earlier problems

### G — Object pools
- G1 object pool with a **union** slot: intrusive free list, placement new,
  occupancy bitset — maximum slot density
- G2 the same pool with `std::optional` slots and a `unique_ptr` handle: 53 lines
  instead of 73, no casts, no manual lifetime — see the measured trade-off in the
  file header

### H — Raw storage and lifetime
- H1 placement new from first principles: `RawStorage<T>` and `RawBlock<T, N>`,
  explicit destruction, exception-safe bulk construction, alignment,
  `std::construct_at` / `destroy_at` / `destroy_n`
- H2 unions: active members, size and alignment, switching a non-trivial member,
  pointer-interconvertibility, the common-initial-sequence rule, and
  `std::variant` as the safe alternative

### I — Memory system effects
- I1 false sharing: same code, counters packed into one cache line vs padded to
  one line each; plain writes and atomic RMW; SMT siblings vs distinct cores;
  read-only sharing for contrast. Threads are pinned so results are stable.
- I2 TLB pressure: miss spikes across the 4 KB/2 MB page-size boundary (4 KB,
  THP, explicit hugetlb), shootdown IPIs provoked by `madvise(MADV_DONTNEED)`
  and counted from `/proc/interrupts`, and page-split loads. Self-measures
  hardware counters via `perf_event_open` (needs `perf_event_paranoid <= 2`).

- I3 page boundary crossing: the comb-shaped latency profile, plotted as ASCII
  from real RDTSC samples bucketed by offset within the page.
- I4 separating the two causes of that spike (TLB miss vs the L2 prefetcher also
  stopping at 4 KB) with a 2x2 design — and reporting why the decomposition is
  untrustworthy, since its control cell fails. Ends with the one robust result:
  a software prefetch removes the whole stall while leaving the miss count alone.
- I5 page-walk cost vs region size: what happens when the page tables themselves
  stop fitting in cache, plus page-order locality and THP grant failures.

These print their own numbers; they are measurements, not assertions, so expect
them to move with machine load. I4 and I5 print a spread or validity column for
exactly that reason — read it before trusting a row.

### J — Atomics and memory ordering
- J1 what each `memory_order` actually compiles to: the program disassembles
  **itself** at run time with objdump, so you see `mov` for relaxed/acquire/
  release, `xchg` for a seq_cst store, `lock xadd` for any RMW, and nothing at
  all for acquire/release fences. Then it catches x86's one permitted reordering
  (StoreLoad) happening in a live litmus test.

- J2 SPSC ring buffer: acquire/release in practice, with the cost of seq_cst,
  index padding (the I1 false-sharing fix) and index caching each measured
  separately — plus the same publish sequence compiled for **x86-64 and
  aarch64** side by side, where `release` becomes `stlr` and `relaxed` does not.

### K — Templates
- K1 mechanics: what deduction keeps and strips, forwarding references and
  reference collapsing, why `std::forward` exists, pack expansion and folds
  (left vs right shown with a non-associative operator), and the same constraint
  written four ways — `void_t`, `enable_if`, `if constexpr`, concepts. Every
  section prints the type it deduced.
- K2 CRTP and its C++23 replacement: classic CRTP, its real costs (unrelated
  base types, unchecked `static_cast`, aggregate-init gotcha), mixins, a
  virtual-dispatch comparison, and the deducing-this version behind
  `#if __cpp_explicit_this_parameter` — needs GCC 14+, so it does not compile
  on GCC 10 and the program says so.
- K3 template bloat, measured: the program reads its own symbol table with `nm`
  to compare a fat template against a thin wrapper over a shared body, counts
  instantiations for recursive packs vs folds, and times real compiles against
  instantiation count.

J- and K-series targets are compiled `-O2` (see `CMakeLists.txt`); the rest of the
workbook is deliberately unoptimised so source and behaviour line up.

The project uses C++20 because several examples use `std::erase`, `std::ssize`, and concepts.
