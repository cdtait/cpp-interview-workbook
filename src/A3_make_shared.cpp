#include <iostream>
#include <memory>
#include <stdexcept>

struct Widget {
    Widget() { std::cout << "Widget constructed\n"; }
    ~Widget() { std::cout << "Widget destroyed\n"; }
};

int mayThrow(bool doThrow) {
    if (doThrow)
        throw std::runtime_error("mayThrow");
    return 42;
}

void f(std::shared_ptr<Widget>, int) {}

int main() {
    std::cout << "Preferred form:\n";
    try {
        f(std::make_shared<Widget>(), mayThrow(false));
    } catch (...) {}

    std::cout << "\nRule: do not expose a raw new in a larger expression.\n";
    std::cout << "The worksheet's leak scenario applies to pre-C++17 evaluation ordering.\n";
}
