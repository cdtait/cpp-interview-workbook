#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

int main() {
    std::vector<int> v{1, 2, 3, 2, 4};
    std::erase(v, 2);

    std::cout << "vector: ";
    for (int x : v) std::cout << x << ' ';
    std::cout << '\n';

    std::map<int, int> m{{1,1}, {2,2}, {3,3}};
    for (auto it = m.begin(); it != m.end();) {
        if (it->first == 2)
            it = m.erase(it);
        else
            ++it;
    }

    std::cout << "map: ";
    for (const auto& [k, value] : m)
        std::cout << '{' << k << ',' << value << "} ";
    std::cout << '\n';
}
