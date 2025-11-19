#pragma once

#include <assert.h>
#include <atomic>
#include <cstddef>

namespace utils::memory {

template <typename T>
concept Allocator = requires(T& a) {
    // 分配指定大小的普通内存块。
    { a.Allocate(size_t{}) } -> std::same_as<char*>;
    // 分配指定大小的对齐内存块。
    // huge_page_size 参数暗示了可能支持大页内存（Huge Pages）以减少TLB压力。
    { a.AllocateAligned(size_t{}) } -> std::same_as<char*>;
    { a.AllocateAligned(size_t{}, size_t{}) } -> std::same_as<char*>;
    { a.AllocateAligned(size_t{}, size_t{}, nullptr) } -> std::same_as<char*>;
    // 返回由该分配器分配的内存块的标准大小。
    // 统一管理和池化内存块，特别是用于 MemTable或 BlockBasedTable的缓存。
    { a.BlockSize() } -> std::same_as<size_t>;
};

/*
convertible_to：强调存在转换关系，A 可通过某种规则（如类型转换、格式转换）变成 B。
+ convertible_to 多用于需要 “转换动作” 的场景，比如编程中的类型兼容、数据格式转换。
same_as：强调本质或属性完全一致，A 和 B 是同一对象、同一概念或完全等价。
+ same_as 多用于身份、属性、内容完全匹配的场景，比如概念定义、对象比对、信息去重。

using A = double，A和double是same_as关系
*/

class WriteBufferManager;

class AllocTracker {
   public:
    explicit AllocTracker(WriteBufferManager* write_buffer_manager);
    // No copying allowed
    AllocTracker(const AllocTracker&) = delete;
    void operator=(const AllocTracker&) = delete;

    ~AllocTracker();
    void Allocate(size_t bytes);
    // Call when we're finished allocating memory so we can free it from
    // the write buffer's limit.
    void DoneAllocating();

    void FreeMem();

    bool is_freed() const { return write_buffer_manager_ == nullptr || freed_; }

   private:
    WriteBufferManager* write_buffer_manager_;
    std::atomic<size_t> bytes_allocated_;
    bool done_allocating_;
    bool freed_;
};

}  // namespace utils::memory