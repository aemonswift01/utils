
#include <concepts>
#ifdef __BMI2__
#include <immintrin.h>
#endif

/*
当使用 -mbmi2 编译标志时，编译器会自动定义 __BMI2__：

bmi2:Bit Manipulation Instruction Set 2
x86 指令集的扩展，专门用于高效的位操作。
*/

namespace utils::utils {
// 保留最低 nbits 位，高位设置为0
/*
template <std::integral T>
等价于

template <typename T>
    requires std::is_integral_v<std::remove_reference_t<T>>
*/
template <std::integral T>
inline T BottomNBits(T v, int nbits) {
    assert(nbits >= 0);
    assert(nbits < int{8 * sizeof(T)});
#ifdef __BMI2__
    if constexpr (sizeof(T) <= 4) {
        return static_cast<T>(_bzhi_u32(static_cast<uint32_t>(v), nbits));
    }
    if constexpr (sizeof(T) <= 8) {
        return static_cast<T>(_bzhi_u64(static_cast<uint64_t>(v), nbits));
    }

#endif
    return static_cast<T>(v & ((T{1} << nbits) - 1));
}

}  // namespace utils::utils