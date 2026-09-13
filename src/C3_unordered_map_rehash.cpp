#include <iostream>
#include <unordered_map>

int main() {
    std::unordered_map<int, int> m;
    m.reserve(2);
    m[1] = 10;

    int* p = &m[1];
    auto* before = p;

    for (int i = 2; i < 200; ++i)
        m[i] = i;

    std::cout << "*p = " << *p << '\n';
    std::cout << "same address after rehash = "
              << std::boolalpha << (before == &m[1]) << '\n';

    std::cout << "Pointers/references survive unordered_map rehash;\n"
                 "iterators do not.\n";
}
