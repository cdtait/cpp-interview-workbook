#include <iostream>

int main() {
    std::cout <<
R"(Benchmarking checklist

- Measure before and after.
- Use realistic inputs.
- Report a distribution, not only a mean.
- For latency-sensitive systems inspect p99 / p99.9.
- Prevent the optimiser from deleting benchmark work.
- Use benchmark::DoNotOptimize when using Google Benchmark.
- Inspect generated assembly when results are surprising.
- Measure cache misses, branches and cycles with perf.
- Be prepared for the measurement to disprove your intuition.
)";
}
