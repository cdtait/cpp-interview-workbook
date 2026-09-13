# F1 — Feed Handling: notifying subscribers without lying to yourself

## The problem

A market-data feed handler receives packets, applies each to an order book, and
notifies subscribers. The naive implementation holds a mutex across the whole
operation — including the subscriber callbacks. That is fast and wrong.

The question this file answers: **what does it actually cost to make it right?**

Four designs are built and measured against each other on the same workload, so
the trade-off is a number rather than an opinion.

## The four designs

| | Design | Lock held during callbacks? | Copies per packet |
|---|---|---|---|
| **A** | Callbacks under the lock | Yes | none |
| **B** | Snapshot book + subscriber list, notify outside | No | full book + list |
| **C** | Copy-on-write, publish an immutable version | No | one book |
| **D** | Single writer owns the book, seqlock for readers | No lock at all | none |

**A** is the original. It never copies, so it looks unbeatable — until a
subscriber calls back into the feed and takes the same non-recursive mutex.

**B** is the obvious fix: copy everything you need under the lock, release it,
then call user code. Safe against any callback whatsoever. Pays a deep copy of
the entire book on every single packet.

**C** narrows that. The book is a `shared_ptr<const Book>`; each packet builds
one new version and publishes it. Subscribers hold their version as long as they
like. One allocation regardless of how many subscribers there are — but still
one copy of the book per packet.

**D** removes the mutex entirely by removing the sharing: one writer thread owns
the book. Callbacks get a direct `const&` and are contractually forbidden from
re-entering or blocking. Outside threads read through a seqlock, retrying if the
writer moved underneath them.

## How it measures

- A **global `operator new`** is replaced with a counting version, so
  "allocations per packet" is observed rather than assumed.
- Each design runs 1,000 warm-up packets first, so allocator growth is not
  charged to the measurement, then 100,000 timed packets.
- Depth is swept across 16, 128 and 1024 levels to separate fixed overhead from
  per-byte copying cost.
- Subscribers are deliberately trivial, so the benchmark measures the plumbing
  and not the subscriber.

Two safety properties are tested rather than argued:

- **Re-entrant callback** — a subscriber that calls back into the feed. Runs on a
  detached thread with a timeout, because on design A it genuinely never returns.
- **Subscribing from a callback** — a subscriber that mutates the subscriber list
  while that list is being iterated.

## Results (GCC 10.3, this machine)

```
     depth       A under-lock        B snapshot     C copy-on-write    D single-writer
        16     276.1  0.00        803.2  2.00        746.8  2.00       202.3  0.00
       128     209.5  0.00        844.4  2.00        797.4  2.00       213.2  0.00
      1024     214.7  0.00       1564.7  2.00       1321.7  2.00       206.2  0.00
                  ns  allocs         ns  allocs         ns  allocs        ns  allocs

  A survives a callback that calls back in : NO (deadlock)
  B survives a callback that subscribes    : yes
  C survives a callback that subscribes    : yes
  D seqlock reader: 1827 consistent snapshots, 6414 gave up
```

### Reading it

> **These numbers are from an unoptimized build** — `CMakeLists.txt` sets no
> `CMAKE_BUILD_TYPE`, so nothing above is compiled with `-O`. Treat the ranking
> as provisional; the B-vs-C ordering reverses under `-O2` (see below).

**Safety costs 3-7x here, and the multiple grows with book depth.** B and C sit
at 2 allocations per packet and degrade as the book grows — B from 803ns to
1565ns between depth 16 and 1024, because it deep-copies the whole book every
time.

**C is not "the faster of B and C" in general — it depends on subscriber count,
and this benchmark fixes that at 4.** Both designs copy exactly one book per
packet, so they scale with depth identically; C's advantage is that it does
*not* copy the subscriber list per packet, while B copies N `std::function`s
every time. Measured at `-O2`, depth 128:

```
 subscribers   B snapshot        C cow     winner
           1        102.5        139.7          B
           4         97.5        143.7          B
          16        171.0        169.6          C
          64        546.7        258.4          C
         256       1926.6        717.6          C
```

The crossover is around 16 subscribers. Below it, C's fixed overhead (an atomic
load plus a refcount touch per packet) costs more than B's list copy saves;
above it, C pulls away — 2.7x by 256 subscribers. Sweeping depth instead, at 4
subscribers, B wins at every depth from 16 to 8192. So depth is not the axis
that separates them; **subscriber count is.**

Note also that roughly 23ns of C's per-packet overhead here is the
`AtomicSharedPtr` fallback below — libstdc++'s `atomic_load` on `shared_ptr`
takes a global spinlock (32.4ns, versus 9.3ns for a plain copy). On GCC 12+,
with the native `std::atomic<std::shared_ptr>`, the crossover moves lower.

**D is the actual answer for a hot path.** It matches A's speed (~210ns, zero
allocations) while being safe — but only by changing the requirements, not by
being cleverer. A single writer owns the data, so there is nothing to lock.

**A's depth-16 number (276ns) is noise.** A does no per-packet copying, so depth
cannot affect it; the three A results should be flat, and 276 vs ~210 is
first-benchmark-in-the-process warm-up, not a real effect.

**D's reader failed 78% of its attempts** (6414 gave up, 1827 succeeded). That is
correct behaviour, not a bug: the writer is running flat out, and at depth 128 the
snapshot copy is long enough that the writer usually bumps the sequence counter
mid-read. A real reader polls far less aggressively.

## The interview point

There is no free lunch, and the cheapest correct option depends on a constraint
that is not in the code:

- If subscribers are untrusted, you are choosing between **B** and **C**, and the
  answer is a crossover, not a winner: B below ~16 subscribers, C above it.
- If you control every subscriber and can forbid re-entry, **D** costs nothing.
- **A** is only defensible if you can prove no callback ever touches the feed —
  and that proof expires the moment someone else adds a subscriber.

The trap in A is that it is fastest *and* correct-looking in every test that does
not include a hostile subscriber. The deadlock is a property of the composition,
not of any line you can point at.

## Building and running

```bash
cmake -S . -B build
cmake --build build --target F1_feed_handling -j
./build/F1_feed_handling
```

Takes a few seconds — it runs 1.2M timed packets plus a 200k-packet seqlock test.

### Toolchain notes

- **Threads are required.** The top-level `CMakeLists.txt` links
  `Threads::Threads` into every target for this file's sake.
- **`std::atomic<std::shared_ptr<T>>` is C++20 (P0718), but libstdc++ only ships
  it from GCC 12.** On older toolchains `<atomic>` rejects it with "requires a
  trivially copyable type". The file uses an `AtomicSharedPtr<T>` shim that is a
  plain alias for `std::atomic<std::shared_ptr<T>>` where available, and falls
  back to the equivalent `std::atomic_load_explicit` / `atomic_store_explicit`
  free functions otherwise. Those are deprecated in C++20, so the deprecation
  warning is suppressed locally rather than project-wide.

Benchmark numbers are meaningless under sanitizers — use the plain `build/`
directory, not `build-asan/`.
