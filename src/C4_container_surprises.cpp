#include <iostream>
#include <map>
#include <string>
#include <vector>

int main() {
    std::map<std::string, int> m;
    int v = m["missing"];

    std::cout << "(1) v = " << v << ", m.size() = " << m.size() << '\n';

    std::vector<int> e;
    auto n = e.size() - 1;
    std::cout << "(2) e.size() - 1 = " << n << '\n';

    std::vector<bool> vb{true, false};

    auto r = vb[0]; // proxy object by value: this is OK
    std::cout << "(3) auto r = vb[0] -> " << static_cast<bool>(r) << '\n';

    // auto& bad = vb[0]; // does not compile: vb[0] returns a proxy temporary
}
