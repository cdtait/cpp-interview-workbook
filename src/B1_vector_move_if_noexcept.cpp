#include <iostream>
#include <string>
#include <vector>

struct A {
    std::string s;
    A() : s("A") {}
    A(const A& other) : s(other.s) { ++copies; }
    A(A&& other) noexcept : s(std::move(other.s)) { ++moves; }

    static inline std::size_t copies = 0;
    static inline std::size_t moves = 0;
};

struct B {
    std::string s;
    B() : s("B") {}
    B(const B& other) : s(other.s) { ++copies; }
    B(B&& other) : s(std::move(other.s)) { ++moves; } // not noexcept

    static inline std::size_t copies = 0;
    static inline std::size_t moves = 0;
};

int main() {
    std::vector<A> va;
    std::vector<B> vb;

    for (int i = 0; i < 1000; ++i) va.emplace_back();
    for (int i = 0; i < 1000; ++i) vb.emplace_back();

    std::cout << "A: copies=" << A::copies << ", moves=" << A::moves << '\n';
    std::cout << "B: copies=" << B::copies << ", moves=" << B::moves << '\n';
}
