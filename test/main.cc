#include <iostream>

#include <cstddef>
#include <new>

class A {
   public:
    struct alignas(std::hardware_destructive_interference_size) {
        size_t shard_block_size_;
    };

    double dummy_;
};

int main(void) {

    std::cout << sizeof(A) << std::endl;
    return 0;
}