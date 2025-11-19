#include "arena.h"
#include <assert.h>

#include "common.h"
#include "log/logging.h"
#include "port/mmap.h"
#include "utils/string_utils.h"

namespace utils::memory {

size_t Arena<N>::OptimizeBlockSize(size_t block_size) {
    // Make sure block_size is in optimal range
    block_size = std::max(Arena::kMinBlockSize, block_size);
    block_size = std::min(Arena::kMaxBlockSize, block_size);

    // make sure block_size is the multiple of kAlignUnit
    if (block_size % kAlignUnit != 0) {
        block_size = (1 + block_size / kAlignUnit) * kAlignUnit;
    }

    return block_size;
}

Arena<N>::Arena(size_t block_size, AllocTracker* tracker, size_t huge_page_size)
    : kBlockSize(OptimizeBlockSize(block_size)), tracker_(tracker) {
    assert(kBlockSize >= kMinBlockSize && kBlockSize <= kMaxBlockSize &&
           kBlockSize % kAlignUnit == 0);
    alloc_bytes_remaining_ = sizeof(inline_block_);
    blocks_memory_ += alloc_bytes_remaining_;
    // 对齐侧指向了低地址
    aligned_alloc_ptr_ = inline_block_;
    // 不对齐侧指向了高地址
    unaligned_alloc_ptr_ = inline_block_ + alloc_bytes_remaining_;
    if constexpr (port::MemMapping::kHugePageSupported) {
        hugetlb_size_ = huge_page_size;
        if (hugetlb_size_ && kBlockSize > hugetlb_size_) {
            // 针对kBlockSize，则需要安装大页对齐
            hugetlb_size_ =
                ((kBlockSize - 1U) / hugetlb_size_ + 1U) * hugetlb_size_;
        }
    }
    if (tracker_ != nullptr) {
        tracker_->Allocate(kInlineSize);
    }
}

/*
大页内存（Huge Page）的分配有严格的对齐要求：
分配的内存大小必须是系统大页粒度（hugetlb_size_，例如 2MB、1GB）的整数倍。
若 kBlockSize（Arena 管理的块大小）大于 hugetlb_size_，直接使用原始 hugetlb_size_ 可
能无法容纳 kBlockSize（因为 kBlockSize 更大），此时需要将大页大小 “扩容” 为足够覆盖 kBlockSize 的最小倍数。
*/

Arena<N>::~Arena() {
    if (tracker_ != nullptr) {
        assert(tracker_->is_freed());
        tracker_->FreeMem();
    }
}

char* Arena<N>::AllocateNewBlock(size_t block_bytes) {
    // NOTE: std::make_unique zero-initializes the block so is not appropriate
    // here
    char* block = new char[block_bytes];
    blocks_.push_back(std::unique_ptr<char[]>(block));

    size_t allocated_size;
#ifdef UTILS_MALLOC_USABLE_SIZE
    allocated_size = malloc_usable_size(block);
#ifndef NDEBUG
    // It's hard to predict what malloc_usable_size() returns.
    // A callback can allow users to change the costed size.
    std::pair<size_t*, size_t*> pair(&allocated_size, &block_bytes);
    TEST_SYNC_POINT_CALLBACK("Arena::AllocateNewBlock:0", &pair);
#endif  // NDEBUG
#else
    allocated_size = block_bytes;
#endif  // UTILS_MALLOC_USABLE_SIZE
    blocks_memory_ += allocated_size;
    if (tracker_ != nullptr) {
        tracker_->Allocate(allocated_size);
    }
    return block;
}

char* Arena<N>::AllocateFallback(size_t bytes, bool aligned) {
    if (bytes > kBlockSize / 4) {
        ++irregular_block_num;
        // Object is more than a quarter of our block size.  Allocate it separately
        // to avoid wasting too much space in leftover bytes.
        return AllocateNewBlock(bytes);
    }

    // We waste the remaining space in the current block.
    size_t size = 0;
    char* block_head = nullptr;
    if (port::MemMapping::kHugePageSupported && hugetlb_size_ > 0) {
        size = hugetlb_size_;
        block_head = AllocateFromHugePage(size);
    }
    if (!block_head) {
        size = kBlockSize;
        block_head = AllocateNewBlock(size);
    }
    alloc_bytes_remaining_ = size - bytes;

    if (aligned) {
        aligned_alloc_ptr_ = block_head + bytes;
        unaligned_alloc_ptr_ = block_head + size;
        return block_head;
    } else {
        aligned_alloc_ptr_ = block_head;
        unaligned_alloc_ptr_ = block_head + size - bytes;
        return unaligned_alloc_ptr_;
    }
}

char* Arena<NGROUPS_MAX>::AllocateFromHugePage(size_t bytes) {
    port::MemMapping mm = port::MemMapping::AllocateHuge(bytes);
    auto addr = static_cast<char*>(mm.Get());
    if (addr) {
        huge_blocks_.push_back(std::move(mm));
        blocks_memory_ += bytes;
        if (tracker_ != nullptr) {
            tracker_->Allocate(bytes);
        }
    }
    return addr;
}

char* Arena<N>::AllocateAligned(size_t bytes, size_t huge_page_size,
                                log::Logger* logger) {
    assert(bytes > 0);
    if (port::MemMapping::kHugePageSupported && hugetlb_size_ > 0 &&
        huge_page_size > 0) {
        // Allocate from a huge page TLB table.
        size_t reserved_size =
            ((bytes - 1U) / huge_page_size + 1U) * huge_page_size;

        char* addr = AllocateFromHugePage(reserved_size);
        if (addr == nullptr) {
            LOG_WARN(logger,
                     "AllocateAligned fail to allocate huge TLB pages: %s",
                     utils::errnoStr(errno).c_str());
            //utils::errnoStr(errno)生成的临时对象在语句结束后就销毁。
            // fail back to malloc
        } else {
            return addr;
        }
    }
    size_t current_mod =
        reinterpret_cast<uintptr_t>(aligned_alloc_ptr_) & (kAlignUnit - 1);
    size_t slop = (current_mod == 0 ? 0 : kAlignUnit - current_mod);
    size_t needed = bytes + slop;
    char* result;
    if (needed <= alloc_bytes_remaining_) {
        result = aligned_alloc_ptr_ + slop;
        aligned_alloc_ptr_ += needed;
        alloc_bytes_remaining_ -= needed;
    } else {
        // AllocateFallback always returns aligned memory
        result = AllocateFallback(bytes, true /* aligned */);
    }
    return result;
}

}  // namespace utils::memory

/*
https://zhuanlan.zhihu.com/p/418263657
https://szza.github.io/2022/01/07/rocksdb/MemoryAllocator/2_concurrent_arena/
https://blog.csdn.net/Z_Stand/article/details/121575525

https://zhuanlan.zhihu.com/p/616209332
*/