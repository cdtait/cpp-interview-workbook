#include <iostream>
#include <utility>

struct T {
    explicit T(int) { std::cout << "ctor\n"; }
    T(const T&) { std::cout << "copy\n"; }
    T(T&&) noexcept { std::cout << "move\n"; }
    ~T() { std::cout << "dtor\n"; }
};

T byName(int i) {
    T t(i);
    return t;
}

T byMove(int i) {
    T t(i);
    return std::move(t); // pessimizing move
}

T byPrvalue(int i) {
    return T(i);
}

int main() {
    std::cout << "byName:\n";
    { auto x = byName(1); }

    std::cout << "\nbyMove:\n";
    { auto x = byMove(2); }

    std::cout << "\nbyPrvalue:\n";
    { auto x = byPrvalue(3); }
}
