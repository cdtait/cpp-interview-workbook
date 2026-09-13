#include <cstdio>

struct B {
    virtual const char* who() const { return "B"; }
};

struct D : B {
    const char* who() const override { return "D"; }
};

int main() {
    D d;

    B byValue = d;
    B& byRef = d;

    std::puts(byValue.who());
    std::puts(byRef.who());
}
