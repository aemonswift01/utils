#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "allocator.h"

#include "arena.h"
#include "core_local.h"
#include "log/logger.h"
#include "mutex/spin_lock.h"
#include "port/const.h"
#include "utils/noncopyable.h"

#include <new>

namespace utils::memory {
// 基于 Arena 实现的线程安全内存分配器。它通过核心本地缓存（per-core cache） 和自旋锁
//（spinlock） 减少多线程分配内存时的竞争，同时通过延迟实例化缓存和动态调整缓存块大小，
// 避免内存浪费。适用于高并发场景下的小内存分配，提升分配效率。
// concurrentArena 是典型的 “分片 + 主 arena” 两级内存池，设计初衷是：
//   大部分 “小内存分配” 走 shard（每个 CPU 核心 /线程绑定一个分片），用线程本地无锁逻辑，极致提升并发性能；
//   少数 “大内存分配”或 “特殊场景” 走 主arena，用全局锁保护，避免分片内存浪费或分配失败。
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
        std::lock_guard<mutex::SpinLock> lock(arena_mutex_);
        // // 仅构造，不加锁
        // std::unique_lock<mutex::SpinLock> lock(arena_mutex_, std::defer_lock);
        // lock.lock();  // 显式加锁
        return arena_.ApproximateMemoryUsage() - ShardAllocatedAndUnused();
        // lock 析构时自动解锁 - 临界区结束
    }

    /*
     std::unique_lock主要用于自动管理RAII
     相比于 std::lock_guard，std::unique_lock 提供更多控制选项：
     +  std::defer_lock --延迟上锁
     ```cpp
     std::unique_lock<std::mutex> lock(mtx, std::defer_lock);
    // 此时锁未被获取，需要手动调用 lock()
     ```
    + std::try_to_lock
    ```cpp
    std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
    if (lock.owns_lock()) {
        // 成功获取锁
    } else {
        // 获取锁失败
    }
    ```
    + std::chrono::milliseconds(100)

    ```cpp
    std::unique_lock<std::mutex> lock(mtx, std::chrono::milliseconds(100));
    if (lock.owns_lock()) {
        // 在100ms内成功获取锁
    }

    ```
    + 所有权转移
    ```cpp
    std::unique_lock<std::mutex> get_lock() {
    std::mutex mtx;
    std::unique_lock<std::mutex> lock(mtx);
    return lock;  // 所有权可以转移
    }
    ```
    + 与条件变量配合使用
    ```cpp
    std::mutex mtx;
    std::condition_variable cv;
    bool data_ready = false;

    void consumer() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, []{ return data_ready; });  // 等待时会自动解锁
        // 处理数据...
    }

    void producer() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            data_ready = true;
        }
        cv.notify_one();
    }
    ```
    + 分层锁（避免死锁）
    ```cpp
    std::mutex mtx1, mtx2;

    void safe_operation() {
        // 使用 std::lock 同时锁定多个互斥量，避免死锁
        std::unique_lock<std::mutex> lock1(mtx1, std::defer_lock);
        std::unique_lock<std::mutex> lock2(mtx2, std::defer_lock);
        std::lock(lock1, lock2);  // 同时锁定
        
        // 安全地操作共享资源...
    }
    ```
    */

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
    // struct Shard {
    //     // 填充字节，避免缓存行竞争（64字节缓存行 - 8字节互斥锁 - 8字节指针 - 8字节原子变量 = 40）
    //     char padding[40] ROCKSDB_FIELD_UNUSED;
    //     // 分片的自旋锁，保证线程安全
    //     mutable mutex::SpinLock mutex;
    //     // 分片内空闲内存的起始地址
    //     char* free_begin_;
    //     // 分片内已分配但未使用的字节数（原子变量）
    //     std::atomic<size_t> allocated_and_unused_;

    //     Shard() : free_begin_(nullptr), allocated_and_unused_(0) {}
    // }; 替换为如下
    // free_begin_只是引用内存，内存的所有权仍然输入Arena。因此从Arena分配下一个时
    // 直接覆盖begin指针
    struct alignas(std::hardware_destructive_interference_size) Shard {
        mutable mutex::SpinLock mutex;
        char* free_begin_;
        std::atomic<size_t> allocated_and_unused_;
    };

    static_assert(sizeof(Shard) == std::hardware_destructive_interference_size,
                  "Shard size must be 64 bytes for cache line alignment");

    static_assert(alignof(Shard) == std::hardware_destructive_interference_size,
                  "Shard alignment must be 64 bytes");

    // 线程本地存储的CPU核ID，用于绑定分片
    static thread_local size_t tls_cpuid;

    // // 填充，避免与后续成员共享缓存行（64字节 - 8字节 shard_block_size_ = 56）
    // char padding0[56] ROCKSDB_FIELD_UNUSED;

    // // 每个分片的块大小（主 arena 块大小的一部分）
    // size_t shard_block_size_; 上述两个等价于
    /*
    char padding0[56] ROCKSDB_FIELD_UNUSED;

    size_t shard_block_size_; 

    ...
    char padding1[56] ROCKSDB_FIELD_UNUSED;
    等价于下面：
    1. shard_block_size_独占一个缓存行
    2. ConcurrentArena按照最大对齐，即64字节
    */

    struct alignas(std::hardware_destructive_interference_size) {
        size_t shard_block_size_;
    };

    // 核心本地数组，存储每个核心的分片
    utils::CoreLocalArray<Shard> shards_;

    // 底层主 arena，用于大分配或分片缓存不足时的分配
    Arena<N> arena_;
    // 主 arena 的自旋锁，保护其操作
    mutable mutex::SpinLock arena_mutex_;
    // 主 arena 已分配但未使用的字节数（原子缓存）
    std::atomic<size_t> arena_allocated_and_unused_;
    // 总分配字节数（原子缓存）
    std::atomic<size_t> memory_allocated_bytes_;
    // 不规则块数量（原子缓存）
    std::atomic<size_t> irregular_block_num_;

    // 重新选择一个分片（当当前分片锁竞争时）
    Shard* Repick();

    // 计算所有分片的未使用总字节数
    size_t ShardAllocatedAndUnused() const {
        size_t total = 0;
        for (size_t i = 0; i < shards_.Size(); ++i) {
            total += shards_.AccessAtCore(i)->allocated_and_unused_.load(
                std::memory_order_relaxed);
        }
        return total;
    }

    // 内存分配的核心实现（模板函数，接受主 arena 分配回调）
    template <typename Func>
    char* AllocateImpl(size_t bytes, bool force_arena, const Func& func) {
        size_t cpu;

        // 若分配大小过大、强制使用主 arena，或未检测到并发且主 arena 锁空闲，则直接使用主 arena
        // 这保证了无并发时没有碎片 penalty，仅在需要时启用并发优化
        /*
        为什么第三个条件分片0需要单独拿出来
        ConcurrentArena 的分片设计初衷是 “1 个 CPU 核心绑定 1 个分片”，通过 tls_cpuid（线程本地存储的 CPU 核心 ID）
        让线程优先使用自己绑定核心的分片，实现无锁分配。但实际运行中，线程未必能稳定绑定到核心，此时会触发 “默认分片 fallback”：
        tls_cpuid 的获取逻辑：RocksDB 会通过系统调用（如 Linux 的 sched_getcpu()）获取当前线程正在运行的 CPU 核心 ID，并存入 tls_cpuid。但这个值不是固定的：
            若线程未设置 CPU 亲和性（taskset/sched_setaffinity），操作系统可能调度线程在不同核心间切换，导致 tls_cpuid 动态变化；
            部分场景下（如低权限环境、跨平台兼容），sched_getcpu() 可能调用失败，tls_cpuid 会被初始化为 默认值 0。
        
        条件3：0 的分片是 “默认分片”（可能被更多线程共享，或缓存更容易耗尽），当它自身无可用内存时，尝试 “非阻塞” 地借用主 arena 的内存，避免分片 0 分配失败。
        */
        std::unique_lock<mutex::SpinLock> arena_lock(arena_mutex_,
                                                     std::defer_lock);
        if (bytes > shard_block_size_ / 4 || force_arena ||
            ((cpu = tls_cpuid) == 0 &&
             !shards_.AccessAtCore(0)->allocated_and_unused_.load(
                 std::memory_order_relaxed) &&
             arena_lock.try_lock())) {
            if (!arena_lock.owns_lock()) {  // 若未获取锁，则加锁
                arena_lock.lock();
            }
            auto rv = func();  //主 arena 分配回调
            Fixup();           // 更新原子缓存（主 arena 的状态）
            return rv;
        }

        // 选择一个分片进行分配
        Shard* s = shards_.AccessAtCore(cpu & (shards_.Size() - 1));
        if (!s->mutex.try_lock()) {  // 若当前分片锁竞争，尝试重新选择
            s = Repick();  //因为有可能得不到tls_cpuid，从而是随机选择的
            s->mutex.lock();
        }
        // 用 adopt_lock 接管已获取的锁，即使用功能RAII释放
        std::unique_lock<mutex::SpinLock> lock(s->mutex, std::adopt_lock);

        // 分片内可用内存
        size_t avail = s->allocated_and_unused_.load(std::memory_order_relaxed);
        if (avail < bytes) {  // 分片内存不足，需要从主 arena 分配新块
            // 锁定主 arena
            std::lock_guard<mutex::SpinLock> reload_lock(arena_mutex_);

            // 若主 arena 当前块大小在合理范围内，调整请求大小以避免碎片
            auto exact =
                arena_allocated_and_unused_.load(std::memory_order_relaxed);
            assert(exact == arena_.AllocatedAndUnused());

            // 若主 arena 的内联块有足够空间（小分配优化），直接从内联块分配
            if (exact >= bytes && arena_.IsInInlineBlock()) {
                // 这确保了我们最初的几次小规模分配无需分配任何块。
                // 特别地，这可以防止空的 memtable 占用过大的内存量：
                // 一个 memtable 在创建时分配大约 1 KB 的内存；
                // 我们不希望为此分配完整的 arena 块（通常为几兆字节），
                // 尤其是在存在数千个空 memtable 的情况下。
                auto rv = func();
                Fixup();
                return rv;
            }

            // 内联块不够则从主 arena 分配给分片的块大小（根据主 arena 当前剩余空间动态调整）
            // 若主 arena 剩余空间在 [1/2, 2) * 分片块大小，则直接使用剩余空间
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
            // 若分配大小是指针大小的倍数（对齐），从分片空闲内存起始处分配
            rv = s->free_begin_;
            s->free_begin_ += bytes;
        } else {
            // 非对齐分配，从分片空闲内存末尾分配（避免破坏前序对齐内存）
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
};

}  // namespace utils::memory