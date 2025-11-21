
#pragma once

#include <cstddef>

namespace utils::utils {
/*
std::aligned_storage<sizeof(std::string)>::type storage;
auto* str = reinterpret_cast<std::string*>(&storage);

// 从编译器视角：storage 和 str 指向的对象生命周期不匹配
new(str) std::string("hello");

// 作用域结束：str 的析构被调用，但 storage 继续存在
// 这种不匹配导致各种优化问题

*/

template <size_t Size, size_t ByteAlignment>
struct AlignedMemory {
    alignas(ByteAlignment) std::byte data[Size];
};
}  // namespace utils::utils