#include <cstdio>
#include <iostream>
#include <memory>

struct N {
    std::shared_ptr<N> other;
    ~N() { std::puts("~N"); }
};

struct NFixed {
    std::weak_ptr<NFixed> other;
    ~NFixed() { std::puts("~NFixed"); }
};

int main() {
    std::cout << "Cycle:\n";
    {
        auto a = std::make_shared<N>();
        auto b = std::make_shared<N>();
        a->other = b;
        b->other = a;

        std::cout << "a.use_count() = " << a.use_count() << '\n';
        std::cout << "b.use_count() = " << b.use_count() << '\n';
    }
    std::cout << "No ~N lines were printed: the cycle leaked.\n\n";

    std::cout << "Fixed with weak_ptr:\n";
    {
        auto a = std::make_shared<NFixed>();
        auto b = std::make_shared<NFixed>();
        a->other = b;
        b->other = a;

        std::cout << "a.use_count() = " << a.use_count() << '\n';
        std::cout << "b.use_count() = " << b.use_count() << '\n';
    }
}
