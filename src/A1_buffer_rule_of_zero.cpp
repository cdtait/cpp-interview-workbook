#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

class Buffer {
public:
    explicit Buffer(std::size_t n)
        : data_(std::make_unique<char[]>(n)), size_(n) {}

    void set(const char* text) {
        std::strncpy(data_.get(), text, size_ - 1);
        data_[size_ - 1] = '\0';
    }

    void show(const char* name) const {
        std::cout << name
                  << ": ptr=" << static_cast<const void*>(data_.get())
                  << ", data=\"" << (data_ ? data_.get() : "<moved-from>")
                  << "\"\n";
    }

private:
    std::unique_ptr<char[]> data_;
    std::size_t size_;
};

int main() {
    Buffer a(1024);
    a.set("Hello from buffer A");
    a.show("a");

    // Buffer copy = a; // compile error: ownership is unique

    Buffer b = std::move(a);
    std::cout << "\nAfter move:\n";
    a.show("a");
    b.show("b");
}
