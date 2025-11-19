// https://github.com/alecthomas/go_serialization_benchmarks
// https://zhuanlan.zhihu.com/p/52620657

// https://zhuanlan.zhihu.com/p/1931120499179647603

#include <cstddef>
#include <type_traits>

// AnyType可以转换成任意类型
struct AnyType {
    template <typename T>
        requires(!std::is_array_v<T>)
    operator T();  // 即AnyType类型转为T类型操作符。
};

/*
  requires(!std::is_array_v<T>)  // 不能是C数组方式，因为会和聚合初始化冲突
*/

// 获取能够进行聚合初始化类的成员变量个数
template <typename T>
consteval size_t CountMember(auto&&... Args) {
    if constexpr (!requires { T{Args...}; }) {
        return sizeof...(Args) - 1;
    } else {
        return CountMember<T>(Args..., AnyType{});
    }
}

/*
requires { T{ Args... }; } 是 C++20 引入的约束表达式（requires-expression），
用于编译期检查某个语法构造是否合法。这里的含义是：检查能否用参数包 Args... 通过聚合初始化（T{ ... }）的方式构造一个 T 类型的对象。

聚合初始化的条件：变量的个数一定要小于等于T的成员变量个数。

分支 1：return sizeof...(Args) - 1;
    当 “参数包 Args... 无法初始化 T” 时执行（即参数个数超过 T 的成员数）。
    sizeof...(Args) 计算当前参数包的长度（参数个数）。
    由于参数个数已超出 T 的成员数，因此 T 的成员数为 “当前参数个数 - 1”，返回该值。
分支 2：return CountMember<T>(Args..., AnyType{});
    当 “参数包 Args... 可以初始化 T” 时执行（即参数个数仍未超过 T 的成员数）。
    Args..., AnyType{}：在原有参数包 Args... 后追加一个 AnyType 类型的临时对象。
    AnyType 有一个模板转换运算符（operator T()），可隐式转换为任意类型，确保追加的参数能匹配 T 成员的类型。
    递归调用 CountMember<T>：用新的、更长的参数包继续试探，直到参数个数超出 T 的成员数，触发分支 1 返回结果。
*/

// https://zhuanlan.zhihu.com/p/1931120499179647603
//  https://zhuanlan.zhihu.com/p/165993590
