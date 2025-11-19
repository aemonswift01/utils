#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include "allocator.h"

#include "log/logger.h"
#include "utils/noncopyable.h"

namespace utils::memory {

/*
类Arena 在分配内存时，是以 block 为单位，即每次先分配一个block大小的内存，后续所需bytes大小的内
存时，会先尝试从block 中获取，如果这个block中剩余的可用内存能满足bytes，则从block中划出一部分给
上层使用，否则才从操作系统中获取。

一个block容纳的内存大小，由kBlockSize参数来指定。

现在Arena怎么实现基类中Allocate、AllocateAligned两个接口？
Arena 中有两个指针：aligned_alloc_ptr_、unaligned_alloc_ptr_，当一个block的内存创建完毕时：
+ aligned_alloc_ptr_：指向该block的首地址（低地址），后续用于分配需要对齐的内存；
+ unaligned_alloc_ptr_：指向该block的末地址（高地址），后续用于分配不需要对齐的内存。

*/

/*
template <ShapeConcept T> // 仅接受符合ShapeConcept的类型
void printArea(const T& shape) {
*/

template <int N>
class Arena : private utils::NonCopyable {
   public:
    // No copying allowed
    Arena(const Arena&) = delete;
    void operator=(const Arena&) = delete;

    static constexpr size_t kInlineSize = 2048;
    static constexpr size_t kMinBlockSize = 4096;
    static constexpr size_t kMaxBlockSize = 2u << 30;

    // kAlignUnit：内存对齐单位，基于系统最大对齐要求（保证所有类型的对象都能正确对齐）。
    // alignof(std::max_align_t)
    static constexpr unsigned kAlignUnit = N;
    static_assert((kAlignUnit & (kAlignUnit - 1)) == 0,
                  "Pointer size should be power of 2");

    // huge_page_size: if 0, don't use huge page TLB. If > 0 (should set to the
    // supported hugepage size of the system), block allocation will try huge
    // page TLB first. If allocation fails, will fall back to normal case.
    explicit Arena(size_t block_size = kMinBlockSize,
                   AllocTracker* tracker = nullptr, size_t huge_page_size = 0);
    ~Arena();

    // 功能：分配 bytes 大小的未对齐内存（内存地址无需满足特定对齐要求）。
    // 逻辑：优先从当前活跃块的剩余空间分配，不足则调用 AllocateFallback分配新块。
    char* Allocate(size_t bytes);

    // huge_page_size:若该值 > 0，会尝试从大页 TLB（快表）中分配内存。
    // 传入的参数代表大页 TLB 的页面大小，单位为字节。
    // 分配时会通过 mmap 匿名映射（开启大页功能），将字节数向上取整为页面大小的整数倍，超出部分的空间会被浪费。
    // 若大页分配失败，会降级使用普通内存分配方式。
    // 启用要求
    // 需提前预留大页资源才能成功分配，示例命令：sysctl - w vm.nr_hugepages =20（预留 20 个大页）
    // 详细说明可参考 Linux 官方文档：Documentation/vm/hugetlbpage.txt

    // 注意事项
    // 大页分配可能失败，失败后会自动降级为普通分配。
    // 分配相关日志会输出到指定的日志器（logger）。因此当设置
    // huge_page_tlb_size > 0 时，强烈建议传入日志器。
    // 若未传入日志器，错误信息会直接打印到标准错误输出（stderr）。
    char* AllocateAligned(size_t bytes, size_t huge_page_size = 0,
                          log::Logger* logger = nullptr);

    // 功能：估算已使用的内存总量（已分配且已使用的部分，不含未使用的剩余空间）。
    //计算方式：总分配内存（blocks_memory_） + 块指针数组的内存 - 当前块未使用空间（alloc_bytes_remaining_）。
    size_t ApproximateMemoryUsage() const {
        return blocks_memory_ + blocks_.size() * sizeof(char*) -
               alloc_bytes_remaining_;
    }

    // 功能：返回所有已分配块的总大小（包括未使用的空间）。
    size_t MemoryAllocatedBytes() const { return blocks_memory_; }

    // 功能：返回当前活跃块中未使用的剩余空间大小。
    size_t AllocatedAndUnused() const { return alloc_bytes_remaining_; }

    // 功能：返回 “不规则块” 的数量（当分配请求超过块大小，直接分配与请求大小相同的块，称为不规则块）。
    size_t IrregularBlockNum() const { return irregular_block_num; }

    // 功能：返回当前使用的内存块大小（kBlockSize 是经 OptimizeBlockSize 调整后的值）。
    size_t BlockSize() const { return kBlockSize; }

    // 功能：判断当前是否使用内联块（inline_block_），即未分配任何常规块或大页块。
    bool IsInInlineBlock() const {
        return blocks_.empty() && huge_blocks_.empty();
    }

    // 功能：调整输入的块大小，使其满足：
    // 在[kMinBlockSize, kMaxBlockSize] 范围内。 是 kAlignUnit的倍数（保证块内内存对齐）。
    static size_t OptimizeBlockSize(size_t block_size);

   private:
    /*******内存块存储 */
    // 内联块（初始小分配使用）
    alignas(std::max_align_t) char inline_block_[kInlineSize];
    // Number of bytes allocated in one block
    // 调整后的标准块大小
    const size_t kBlockSize;
    // Allocated memory blocks
    // 常规内存块队列（智能指针管理）
    std::deque<std::unique_ptr<char[]>> blocks_;
    // Huge page allocations
    // 大页内存块队列（通过 mmap 分配）
    std::deque<utils::mmap::MemMapping> huge_blocks_;
    // 不规则块计数器
    size_t irregular_block_num = 0;

    /*******当前块状态（内存池核心管理） */
    // 设计逻辑：当前块分为两个区域，分别从两端分配未对齐和对齐内存，减少对齐导致的内存浪费。
    // Stats for current active block.
    // For each block, we allocate aligned memory chucks from one end and
    // allocate unaligned memory chucks from the other end. Otherwise the
    // memory waste for alignment will be higher if we allocate both types of
    // memory from one direction.
    // 未对齐分配的当前指针（从块的一端开始）
    char* unaligned_alloc_ptr_ = nullptr;
    // 对齐分配的当前指针（从块的另一端开始）
    char* aligned_alloc_ptr_ = nullptr;
    // How many bytes left in currently active block?
    // 当前块剩余可分配空间
    size_t alloc_bytes_remaining_ = 0;

    /*******其他变量*/
    // 大页大小（构造函数传入）
    // 默认情况下使用mmap给block分配的内存大小。如果 hugetlb_size_ == 0，则表示不使用mmap分配内存。
    size_t hugetlb_size_ = 0;

    // 功能：尝试通过大页（hugetlb_size_）分配内存（需字节数向上取整为大页大小的倍数），失败则返回 nullptr。
    char* AllocateFromHugePage(size_t bytes);
    // 功能：当当前块空间不足时，分配新块（若 bytes 超过 kBlockSize 则分配不规则块，否则分配标准块）。
    char* AllocateFallback(size_t bytes, bool aligned);
    //功能：分配一个大小为 block_bytes 的新常规块，更新 blocks_ 和 blocks_memory_，并初始化当前块的指针。
    char* AllocateNewBlock(size_t block_bytes);

    // Bytes of memory in blocks allocated so far
    // 所有常规块的总大小（字节）
    size_t blocks_memory_ = 0;
    // Non-owned
    // 内存分配跟踪器（非所有权）
    AllocTracker* tracker_;
};

template <int N>
inline char* Arena<N>::Allocate(size_t bytes) {
    assert(bytes > 0);
    if (bytes < alloc_bytes_remaining_) {
        unaligned_alloc_ptr_ -= bytes;
        alloc_bytes_remaining_ -= bytes;
        return unaligned_alloc_ptr_;
    }
    return AllocateFallback(bytes, false /* unaligned */);
}
}  // namespace utils::memory
