#include <algorithm>
#include <cstdio>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v{1, 2, 3, 2, 4};

    auto it = std::remove(v.begin(), v.end(), 2);

    std::printf("%zu\n", v.size());

    std::cout << "logical contents: ";
    for (auto p = v.begin(); p != it; ++p)
        std::cout << *p << ' ';
    std::cout << '\n';

    v.erase(it, v.end());

    std::printf("%zu\n", v.size());

    std::cout << "final contents: ";
    for (int x : v)
        std::cout << x << ' ';
    std::cout << '\n';
}
