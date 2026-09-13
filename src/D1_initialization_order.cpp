#include <iostream>

struct S {
    int a;
    int b;

    S(int x)
        : b(x), a(b * 2) {} // warning: a is initialized first
};

int main() {
    std::cout << "This source intentionally contains undefined behaviour.\n";
    std::cout << "Compile with -Wall -Wextra to see the initialization-order warning.\n";

    // Running it is intentionally avoided because reading b before initialization is UB.
    // S s(5);
    // std::cout << "a=" << s.a << ", b=" << s.b << '\n';
}
