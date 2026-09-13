#include <iostream>
#include <string>
#include <utility>
#include <vector>

struct test_add {
    std::vector<std::string> items_;

    void addC(std::string s) {
        items_.push_back(std::move(s));
    }

    void add(const std::string& s) {
        items_.push_back(s);
    }

    void add(std::string&& s) {
        items_.push_back(std::move(s));
    }

    void show(const char* name) const {
        std::cout << name << " [" << items_.size() << " items]\n";
        for (std::size_t i = 0; i < items_.size(); ++i)
            std::cout << "  [" << i << "] \"" << items_[i] << "\"\n";
    }
};

int main() {
    test_add t;

    std::string a = "AAPL";
    std::string b = "MSFT";

    t.add(a);            // lvalue -> const& overload -> copy into vector
    t.add(std::move(b)); // rvalue -> && overload -> move into vector
    t.addC("GOOG");      // by value, then move into vector

    t.show("items");
}
