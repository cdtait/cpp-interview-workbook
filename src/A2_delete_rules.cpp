#include <iostream>

struct Base {
    ~Base() { std::cout << "~Base\n"; }
};

struct Derived : Base {
    ~Derived() { std::cout << "~Derived\n"; }
};

int main() {
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
