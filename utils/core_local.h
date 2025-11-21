#pragma once
#include <cassert>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

#include "port/likely.h"
#include "utils/math.h"
#include "utils/random.h"

namespace utils::utils {

// 核心本地值的数组。理想情况下，值类型 T 应按缓存行对齐，以防止伪共享（false sharing）。
// 分配一个大数组，cpuid的索引映射到数组的索引，从而得到cpu本地存储的内容
template <typename T>
class CoreLocalArray {
   public:
    CoreLocalArray();

    // 返回数组大小（核心数量的向上取整的 2 的幂）
    size_t Size() const;
    // 返回当前线程运行的核心对应的元素指针
    T* Access() const;
    // 同上，但同时返回核心索引。客户端可缓存索引以减少获取核心ID的频率，
    // 但需容忍一定不准确性（线程可能迁移到其他核心）
    std::pair<T*, size_t> AccessElementAndIndex() const;
    // 返回指定核心索引对应的元素指针，可用于聚合操作或客户端缓存核心索引的场景
    T* AccessAtCore(size_t core_idx) const;

   private:
    // 存储核心本地数据的数组（智能指针管理内存）
    std::unique_ptr<T[]> data_;
    // 用于计算数组大小的移位值（数组大小 = 1 << size_shift_）
    int size_shift_;
};

// 分配数组内存（大小为 1 << size_shift_）
template <typename T>
CoreLocalArray<T>::CoreLocalArray() {
    // 获取硬件支持的并发线程数（通常等于物理核心数）
    int num_cpus = static_cast<int>(std::thread::hardware_concurrency());
    // find a power of two >= num_cpus and >= 8
    size_shift_ = 3;
    while (1 << size_shift_ < num_cpus) {
        ++size_shift_;
    }
    data_.reset(new T[static_cast<size_t>(1) << size_shift_]);
}

// 返回数组大小（核心本地元素的总数）
template <typename T>
size_t CoreLocalArray<T>::Size() const {
    return static_cast<size_t>(1) << size_shift_;
}

// 访问当前线程所在核心的元素（返回指针）
template <typename T>
T* CoreLocalArray<T>::Access() const {
    return AccessElementAndIndex().first;
}

// 访问当前线程所在核心的元素，同时返回元素指针和核心索引

template <typename T>
std::pair<T*, size_t> CoreLocalArray<T>::AccessElementAndIndex() const {
    // 获取当前线程运行的物理核心ID（平台相关实现）
    int cpuid = port::PhysicalCoreID();
    size_t core_idx;
    if (UNLIKELY(cpuid < 0)) {
        // cpu id unavailable, just pick randomly
        // 随机选择一个索引（避免空指针）
        core_idx = Random::GetTLSInstance()->Uniform(1 << size_shift_);
    } else {
        // 取 cpuid 的低 size_shift_ 位作为索引（哈希映射到数组范围）
        core_idx = static_cast<size_t>(BottomNBits(cpuid, size_shift_));
    }
    // 返回元素指针和索引（通过 AccessAtCore 获取指定索引的元素）
    return {AccessAtCore(core_idx), core_idx};
}

template <typename T>
T* CoreLocalArray<T>::AccessAtCore(size_t core_idx) const {
    assert(core_idx < static_cast<size_t>(1) << size_shift_);
    return &data_[core_idx];
}
}  // namespace utils::utils