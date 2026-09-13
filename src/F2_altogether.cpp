#include <bits/stdc++.h>


[[nodiscard]] int test1() {
    std::cout << "\ntest1\n";
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
    
    Buffer a(1024);
    a.set("Hello from buffer A");
    a.show("a");

    // Buffer copy = a; // compile error: ownership is unique

    Buffer b = std::move(a);
    std::cout << "\nAfter move:\n";
    a.show("a");
    b.show("b");
    
    return 0;
}

[[nodiscard]] int test2() {
    std::cout << "\ntest2\n";
    
    struct Base {
        ~Base() { std::cout << "~Base\n"; }
    };

    struct Derived : Base {
        ~Derived() { std::cout << "~Derived\n"; }
    };

    int* p = nullptr;
    delete p;
    std::cout << "(a) delete nullptr: OK\n";

    int* q = new int[5];
    delete[] q;
    std::cout << "(b) correct form is new[] + delete[]\n";
    // int* bad_q = new int[5];
    // delete bad_q; // UB

    // Base* b = new Derived();
    // delete b; // UB because Base destructor is not virtual
    std::cout << "(c) deleting Derived through non-virtual Base*: UB\n";

    int* r = new int(1);
    delete r;
    r = nullptr;
    delete r;
    std::cout << "(d) second delete after nulling: OK\n";
    
    return 0;
}

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
    
  void show() const {
    std::cout << "ID\tPrice\n";
    std::cout << "----------------\n";

    for (const auto& item : items_) {
        std::cout << item.id()
                  << '\t'
                  << item.price()
                  << '\n';
    }
}

private:
    std::vector<T> items_;
};

[[nodiscard]] int test3() {
    std::cout << "\ntest3\n";
    
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
    
    Store<Quote> store;
    store.add(Quote{1, 101.25});
    store.add(Quote{2, 99.75});

    std::cout << "total = " << store.total() << '\n';
    store.show();
    
    return 0;
}

struct Widget {
    Widget() { std::cout << "Widget constructed\n"; }
    ~Widget() { std::cout << "Widget destroyed\n"; }
};

int mayThrow(bool doThrow) {
    if (doThrow)
        throw std::runtime_error("mayThrow");
    return 42;
}

void f(std::shared_ptr<Widget>, int) {}

[[nodiscard]] int test4() {
    std::cout << "\ntest4\n";
    std::cout << "Preferred form:\n";
    try {
        f(std::make_shared<Widget>(), mayThrow(false));
        // No construction because of throw
        f(std::make_shared<Widget>(), mayThrow(true));
    } catch (...) {}

    std::cout << "\nRule: do not expose a raw new in a larger expression.\n";
    std::cout << "The worksheet's leak scenario applies to pre-C++17 evaluation ordering.\n";

  return 0;
}

struct N {
    std::shared_ptr<N> other;
    ~N() { std::puts("~N"); }
};

struct NFixed {
    std::weak_ptr<NFixed> other;
    ~NFixed() { std::puts("~NFixed"); }
};

[[nodiscard]] int test5() {
    std::cout << "\ntest5\n";
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
    
    return 0;
}

struct A {
    std::string s;
    A() : s("A") {}
    A(const A& other) : s(other.s) { ++copies; }
    A(A&& other) noexcept : s(std::move(other.s)) { ++moves; }

    static inline std::size_t copies = 0;
    static inline std::size_t moves = 0;
};

struct B {
    std::string s;
    B() : s("B") {}
    B(const B& other) : s(other.s) { ++copies; }
    B(B&& other) : s(std::move(other.s)) { ++moves; } // not noexcept

    static inline std::size_t copies = 0;
    static inline std::size_t moves = 0;
};

[[nodiscard]] int test6() {
   std::cout << "\ntest6\n";
    std::vector<A> va;
    std::vector<B> vb;

    for (int i = 0; i < 1000; ++i) va.emplace_back();
    for (int i = 0; i < 1000; ++i) vb.emplace_back();

    std::cout << "A: copies=" << A::copies << ", moves=" << A::moves << '\n';
    std::cout << "B: copies=" << B::copies << ", moves=" << B::moves << '\n';
  
  return 0;
}


#include <bits/stdc++.h>

struct Counters {
    int copies{0};
    int moves{0};
    void reset() { copies = moves = 0; }
};

inline Counters counters;
 
struct Leg {
    double px{0.0};
    double qty{0.0};
};
 

class OrderFast {
  public:
    OrderFast() = default;
 
    OrderFast(const OrderFast& o)
    : cl_ord_id_{o.cl_ord_id_}
    , legs_{o.legs_}
    {
        ++counters.copies;
    }
 
    OrderFast(OrderFast&& o) noexcept    // <- the only difference
    : cl_ord_id_{std::move(o.cl_ord_id_)}
    , legs_{std::move(o.legs_)}
    {
        ++counters.moves;
    }
 
    OrderFast& operator=(const OrderFast&) = default;
    OrderFast& operator=(OrderFast&&) noexcept = default;
 
  private:
    std::string cl_ord_id_{"CLI-0000000000000001"};
    std::vector<Leg> legs_{{1.0851, 1e6}, {1.0852, 2e6}};
};

class OrderSlow {
  public:
    OrderSlow() = default;
 
    OrderSlow(const OrderSlow& o)
    : cl_ord_id_{o.cl_ord_id_}
    , legs_{o.legs_}
    {
        ++counters.copies;
    }
 
    OrderSlow(OrderSlow&& o)          // <- no noexcept
    : cl_ord_id_{std::move(o.cl_ord_id_)}
    , legs_{std::move(o.legs_)}
    {
        ++counters.moves;
    }
 
    OrderSlow& operator=(const OrderSlow&) = default;
    OrderSlow& operator=(OrderSlow&&) = default;
 
  private:
    std::string cl_ord_id_{"CLI-0000000000000001"};   // long enough to heap-allocate
    std::vector<Leg> legs_{{1.0851, 1e6}, {1.0852, 2e6}};
};

template <typename T>
void count(const char* name, int n)
{
    counters.reset();
    std::vector<T> v;
    for (int i = 0; i < n; ++i) {
        v.push_back(T{});
    }
    std::printf("  %-14s nothrow_move=%d  copyable=%d   copies %3d   moves %3d\n", name,
                std::is_nothrow_move_constructible_v<T>, std::is_copy_constructible_v<T>,
                counters.copies, counters.moves);
}

int test7() {
  count<OrderSlow>("OrderSlow", 8);
  count<OrderFast>("OrderFast", 8);
  
  return 0;
}

int main() {
    int state = test1();
    state = test2();
    state = test3();
    state = test4();
    state = test5();
    state = test6();
    state = test7();
}
