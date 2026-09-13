#include <concepts>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>

template <typename T>
concept Priceable =
    std::is_nothrow_move_constructible_v<T> &&
    requires(const T& t) {
        { t.price() } -> std::convertible_to<double>;
        { t.id() } -> std::same_as<std::uint64_t>;
    };

template <Priceable T>
class Store {
public:
    void add(T t) {
        items_.push_back(std::move(t));
    }

    double total() const {
        double sum = 0.0;
        for (const auto& item : items_)
            sum += item.price();
        return sum;
    }

private:
    std::vector<T> items_;
};

struct Quote {
    std::uint64_t id_;
    double price_;

    Quote(std::uint64_t id, double price) noexcept
        : id_(id), price_(price) {}

    Quote(const Quote&) = default;
    Quote(Quote&&) noexcept = default;

    double price() const { return price_; }
    std::uint64_t id() const { return id_; }
};

int main() {
    Store<Quote> store;
    store.add(Quote{1, 101.25});
    store.add(Quote{2, 99.75});

    std::cout << "total = " << store.total() << '\n';
}
