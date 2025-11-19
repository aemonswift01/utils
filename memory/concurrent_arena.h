#pragma once

#include <atomic>
#include <memory>

#include "allocator.h"

#include "arena.h"
#include "log/logger.h"
#include "mutex/spin_lock.h"
#include "utils/noncopyable.h"

namespace utils::memory {
// 基于 Arena 实现的线程安全内存分配器。它通过核心本地缓存（per-core cache） 和自旋锁
//（spinlock） 减少多线程分配内存时的竞争，同时通过延迟实例化缓存和动态调整缓存块大小，
// 避免内存浪费。适用于高并发场景下的小内存分配，提升分配效率。

template <int N>
class ConcurrentArena : private utils::NonCopyable {
   public:
    // block_size 和 huge_page_size 与 Arena 相同（实际上只是传递给 arena_ 的构造函数）。
    // 核心本地分片的 shard_block_size 计算为 block_size 的一部分，具体比例根据硬件并发级别而定。
    explicit ConcurrentArena(size_t block_size = Arena<N>::kMinBlockSize,
                             AllocTracker* tracker = nullptr,
                             size_t huge_page_size = 0);

    char* Allocate(size_t bytes) override {
        assert(bytes > 0);
        return AllocateImpl(bytes, false /*force_arena*/,
                            [this, bytes]() { return arena_.Allocate(bytes); });
    }

    char* AllocateAligned(size_t bytes, size_t huge_page_size = 0,
                          Logger* logger = nullptr) override {
        assert(bytes > 0);
        size_t rounded_up = ((bytes - 1) | (sizeof(void*) - 1)) + 1;

        return AllocateImpl(rounded_up, huge_page_size != 0 /*force_arena*/,
                            [this, rounded_up, huge_page_size, logger]() {
                                return arena_.AllocateAligned(
                                    rounded_up, huge_page_size, logger);
                            });
    }

    // 近似内存使用量（主 arena 用量减去分片未使用量）。可能又在分配，又在使用
    size_t ApproximateMemoryUsage() const {
        // 传统方式：std::unique_lock<SpinMutex> lock(arena_mutex_);  // 立即获得锁

        // 仅构造，不加锁
        std::unique_lock<mutex::SpinLock> lock(arena_mutex_, std::defer_lock);
        lock.lock();  // 显式加锁
        return arena_.ApproximateMemoryUsage() - ShardAllocatedAndUnused();
        // lock 析构时自动解锁 - 临界区结束
    }

    //   // 返回已分配的总字节数（原子操作，无锁）
    size_t MemoryAllocatedBytes() const {
        return memory_allocated_bytes_.load(std::memory_order_relaxed);
    }

    // 返回已分配但未使用的字节数（主 arena + 分片缓存）
    size_t AllocatedAndUnused() const {
        return arena_allocated_and_unused_.load(std::memory_order_relaxed) +
               ShardAllocatedAndUnused();
    }

    // 返回不规则块的数量（原子操作，无锁）
    size_t IrregularBlockNum() const {
        return irregular_block_num_.load(std::memory_order_relaxed);
    }

    // 返回主 arena 的块大小
    size_t BlockSize() const override { return arena_.BlockSize(); }

   private:
    // 核心本地缓存分片结构，每个核一个
    struct Shard {
        // 填充字节，避免缓存行竞争（64字节缓存行 - 8字节互斥锁 - 8字节指针 - 8字节原子变量 = 40）
        char padding[40] ROCKSDB_FIELD_UNUSED;
        // 分片的自旋锁，保证线程安全
        mutable mutex::SpinLock mutex;
        // 分片内空闲内存的起始地址
        char* free_begin_;
        // 分片内已分配但未使用的字节数（原子变量）
        std::atomic<size_t> allocated_and_unused_;

        Shard() : free_begin_(nullptr), allocated_and_unused_(0) {}
    };

    // 线程本地存储的CPU核ID，用于绑定分片
    static thread_local size_t tls_cpuid;
    // 填充，避免与后续成员共享缓存行（64字节 - 8字节 shard_block_size_ = 56）
    char padding0[56] ROCKSDB_FIELD_UNUSED;

    // 每个分片的块大小（主 arena 块大小的一部分）
    size_t shard_block_size_;

    // 核心本地数组，存储每个核心的分片
    CoreLocalArray<Shard> shards_;

    Arena arena_;
    mutable mutex::SpinLock arena_mutex_;
    std::atomic<size_t> arena_allocated_and_unused_;
    std::atomic<size_t> memory_allocated_bytes_;
    std::atomic<size_t> irregular_block_num_;

    char padding1[56] ROCKSDB_FIELD_UNUSED;

    Shard* Repick();

    size_t ShardAllocatedAndUnused() const {
        size_t total = 0;
        for (size_t i = 0; i < shards_.Size(); ++i) {
            total += shards_.AccessAtCore(i)->allocated_and_unused_.load(
                std::memory_order_relaxed);
        }
        return total;
    }

    template <typename Func>
    char* AllocateImpl(size_t bytes, bool force_arena, const Func& func) {
        size_t cpu;

        // Go directly to the arena if the allocation is too large, or if
        // we've never needed to Repick() and the arena mutex is available
        // with no waiting.  This keeps the fragmentation penalty of
        // concurrency zero unless it might actually confer an advantage.
        std::unique_lock<SpinMutex> arena_lock(arena_mutex_, std::defer_lock);
        if (bytes > shard_block_size_ / 4 || force_arena ||
            ((cpu = tls_cpuid) == 0 &&
             !shards_.AccessAtCore(0)->allocated_and_unused_.load(
                 std::memory_order_relaxed) &&
             arena_lock.try_lock())) {
            if (!arena_lock.owns_lock()) {
                arena_lock.lock();
            }
            auto rv = func();
            Fixup();
            return rv;
        }

        // pick a shard from which to allocate
        Shard* s = shards_.AccessAtCore(cpu & (shards_.Size() - 1));
        if (!s->mutex.try_lock()) {
            s = Repick();
            s->mutex.lock();
        }
        std::unique_lock<SpinMutex> lock(s->mutex, std::adopt_lock);

        size_t avail = s->allocated_and_unused_.load(std::memory_order_relaxed);
        if (avail < bytes) {
            // reload
            std::lock_guard<SpinMutex> reload_lock(arena_mutex_);

            // If the arena's current block is within a factor of 2 of the right
            // size, we adjust our request to avoid arena waste.
            auto exact =
                arena_allocated_and_unused_.load(std::memory_order_relaxed);
            assert(exact == arena_.AllocatedAndUnused());

            if (exact >= bytes && arena_.IsInInlineBlock()) {
                // If we haven't exhausted arena's inline block yet, allocate from arena
                // directly. This ensures that we'll do the first few small allocations
                // without allocating any blocks.
                // In particular this prevents empty memtables from using
                // disproportionately large amount of memory: a memtable allocates on
                // the order of 1 KB of memory when created; we wouldn't want to
                // allocate a full arena block (typically a few megabytes) for that,
                // especially if there are thousands of empty memtables.
                auto rv = func();
                Fixup();
                return rv;
            }

            avail =
                exact >= shard_block_size_ / 2 && exact < shard_block_size_ * 2
                    ? exact
                    : shard_block_size_;
            s->free_begin_ = arena_.AllocateAligned(avail);
            Fixup();
        }
        s->allocated_and_unused_.store(avail - bytes,
                                       std::memory_order_relaxed);

        char* rv;
        if ((bytes % sizeof(void*)) == 0) {
            // aligned allocation from the beginning
            rv = s->free_begin_;
            s->free_begin_ += bytes;
        } else {
            // unaligned from the end
            rv = s->free_begin_ + avail - bytes;
        }
        return rv;
    }

    void Fixup() {
        arena_allocated_and_unused_.store(arena_.AllocatedAndUnused(),
                                          std::memory_order_relaxed);
        memory_allocated_bytes_.store(arena_.MemoryAllocatedBytes(),
                                      std::memory_order_relaxed);
        irregular_block_num_.store(arena_.IrregularBlockNum(),
                                   std::memory_order_relaxed);
    }

    ConcurrentArena(const ConcurrentArena&) = delete;
    ConcurrentArena& operator=(const ConcurrentArena&) = delete;
};

}  // namespace utils::memory