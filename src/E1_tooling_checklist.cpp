#include <iostream>

int main() {
    std::cout <<
R"(Production-only bug checklist

1. Sanitizers
   - AddressSanitizer + UndefinedBehaviorSanitizer
   - ThreadSanitizer in a separate build

2. Warnings
   -Wall -Wextra -Wpedantic
   -Wreorder -Wuninitialized
   -Wpessimizing-move -Wreturn-local-addr

3. Compare optimisation levels
   Debug-only / release-only differences often expose UB.

4. Performance tools
   perf stat
   perf record
   perf report

5. Put sanitizer builds in CI.
)";
}
