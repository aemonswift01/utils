#include <cstdint>
#include <type_traits>

namespace utils::hash {
namespace {  //static
template <typename T>
using has_func_member = decltype(std::declval<T>().hash());

template <typename T>
std::true_type is_hash_func_member(has_func_member<T>*);

template <typename T>
std::false_type is_hash_func_member(...);

template <typename T>
struct HasHashFuncMember : decltype(is_hash_func_member<T>(nullptr)) {};
}  // namespace

template <typename T>
uint64_t common_hash(const T& key) {
    return std::hash<T>{}(key);
}

template <typename T>
inline uint64_t hash(const T& key) {
    if constexpr (std::is_integral_v<T>) {
        return static_cast<uint64_t>(key);
    } else if constexpr (std::is_floating_point_v<T>) {
        double v = static_cast<double>(key);
        return *reinterpret_cast<uint64_t*>(&v);
    } else if constexpr (detail::HasHashFuncMember<T>::value) {
        return key.hash();
    } else {
        return common_hash(key);
    }
}
}  // namespace utils::hash
