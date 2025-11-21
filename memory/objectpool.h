#pragma once

#include <cstddef>
#include <new>
#include "utils/aligned_storage.h"

namespace utils::memory {
template <typename T>
class alignas(std::hardware_destructive_interference_size) ResourcePool {
   public:
    /*
    // 对于小对象：限制数量，避免分配过大块
    // SmallObject(16B): 256个 * 16B = 4KB (而不是64KB)

    // 对于大对象：限制大小，避免单个块占用过多内存
    // LargeObject(8KB): 8个 * 8KB = 64KB (而不是2048个)

    // 对于小对象：限制数量，避免分配过大块
    // SmallObject(16B): 256个 * 16B = 4KB (而不是64KB)

    // 对于大对象：限制大小，避免单个块占用过多内存
    // LargeObject(8KB): 8个 * 8KB = 64KB (而不是2048个)
   */
    static constexpr size_t blockMaxSize = 64 * 1024;  // bytes
    static constexpr size_t blockMaxItem = 256;
    static constexpr size_t freeChunkItem = GetBlockItem();

    consteval size_t GetBlockItem() {
        size_t N1 = blockMaxSize / sizeof(T);
        N1 = N1 < 1 ? 1 : N1;
        return N1 > blockMaxItem ? blockMaxItem : N1;
    }

    using FreeChunk = ResourcePoolFreeChunk<T, freeChunkItem>;
    using DynamicFreeChunk = ResourcePoolFreeChunk<T, 0>;

    using BlockItem =

        struct alignas(std::hardware_destructive_interference_size) Block {
        BlockItem items_[freeChunkItem];
        size_t nitem_;

        Block() : nitem(0) {}
    };

   private:
};

template <typename T, size_t nitem>
struct ResourcePoolFreeChunk {
    size_t nfree_;
    ResourceId<T> ids_[nitem];
};

template <typename T>
struct ResourceId {
    uint64_t value_;

    operator uint64_t() const { return value_; }

    // 允许你将一个类型的 ResourceId 转换为另一个类型的 ResourceId，保持底层数值不变，只改变类型信息。
    template <typename T2>
    ResourceId<T2> cast() const {
        ResourceId<T2> id = {value_};  //聚合初始化
        return id;
    }
};
}  // namespace utils::memory
