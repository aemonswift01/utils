#include <type_traits>
#include <functional>
#include <cstdint>

namespace utils
{
    namespace hash
    {
        namespace detail
        {
            template <typename T>
            using has_func_member = decltype(std::declval<T>().hash());
            /*
            std::declval<T>()：在未构造对象的情况下，生成一个 T 类型的 “伪对象”（仅用于编译期类型检查，不能在运行时使用）。
            .hash()：尝试调用这个伪对象的 hash() 成员函数。
            decltype(...)：获取上述表达式的类型。如果 T 确实有 hash() 成员，则表达式有效，
            has_func_member就是hash的返回值类型
            如果 T 没有 hash() 成员函数，那么 std::declval<T>().hash() 是非法的表达式，此时 has_member_hash_t<T>
            会因编译错误而无法被实例化（但由于后续配合 SFINAE 机制，这个错误会被转化为 “匹配失败”，而非编译报错）。
            */

            template <typename T>
            std::true_type is_hash_func_member(has_func_member<T> *);
            /*
            只有当 has_member_hash_t<T> 是有效类型（即 T 有 hash() 成员）时，这个函数的参数类型才合法，该重载才会被编译器考虑。
            例如 has_func_member<T>为void。则为void*
            */
            template <typename T>
            std::false_type is_hash_func_member(...);
            /*
            SFINAE的回退方式，
            参数是 ...（C 风格的可变参数），它的匹配优先级是最低的（仅当其他重载都无法匹配时才会被选中）。
            */
            template <typename T>
            struct HasHashFuncMember : decltype(is_hash_func_member<T>(nullptr))
            {
            };
            /*
            decltype(detail::test_hash<T>(nullptr)) 得到是std::false_type/std::false_type
            继承后，HasHashMember<T> 会拥有 std::true_type 或 std::false_type 的所有特性，
            尤其是静态成员 value（bool 类型），可以通过 HasHashMember<T>::value 获取判断结果。
            */
        } // detail

        template <typename T>
        uint64_t common_hash(const T &key)
        {
            return std::hash<T>{}(key);
        }

        template <typename T>
        inline uint64_t hash(const T &key)
        {
            if constexpr (std::is_integral_v<T>)
            {
                return static_cast<uint64_t>(key);
            }
            else if constexpr (std::is_floating_point_v<T>)
            {
                double v = static_cast<double>(key);
                return *reinterpret_cast<uint64_t *>(&v);
            }
            else if constexpr (detail::HasHashFuncMember<T>::value)
            {
                return key.hash();
            }
            else
            {
                return common_hash(key);
            }
        }

    } // hash

} // utils