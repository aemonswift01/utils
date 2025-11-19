
#pragma once

#include <cstddef>

namespace utils::utils {
template <typename T, std::size_t Align = alignof(T)>
struct aligned_storage {
    struct type {
        alignas(Align) unsigned char data[sizeof(T)];
    };
};
}  // namespace utils::utils