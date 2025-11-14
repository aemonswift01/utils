
#include <cstdint>
#include "hash.h"
#include <iostream>

struct MyStruct
{
    int data = 42;
    uint64_t hash() const
    {
        return static_cast<uint64_t>(data * 31); // 自定义哈希算法
    }
};

int main(int argc, char **argv)
{
    MyStruct s;
    auto v = utils::hash::hash(s);
    int a = 14;

    std::cout << v << utils::hash::hash(a) << std::endl;
}