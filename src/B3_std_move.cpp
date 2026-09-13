#include <iostream>
#include <string>
#include <utility>

int main() {
    std::string a = "hello";
    std::string b = std::move(a);

    const std::string c = "world";
    std::string d = std::move(c); // copies: move ctor cannot take const string&&

    std::cout << "b = " << b << '\n';
    std::cout << "d = " << d << '\n';

    std::cout << "a after move is valid but its value is unspecified.\n";
    a = "reassigned";
    std::cout << "a after reassignment = " << a << '\n';
}
