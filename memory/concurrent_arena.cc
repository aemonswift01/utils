#include "memory/concurrent_arena.h"

namespace utils::memory {

namespace {  // static
// 如果分片块大小过大，在最坏情况下，每个核心都会分配一个块但不填充数据。
// 如果共享块大小为 1MB，64 个核心将快速分配 64MB，
// 并可能迅速触发刷写操作。因此需要限制其大小。
constexpr size_t kMaxShardBlockSize = size_t{128 * 1024};
}  // namespace

thread_local size_t ConcurrentArena<N>::tls_cpuid = 0;

ConcurrentArena<N>::ConcurrentArena(size_t block_size, AllocTracker* tracker,
                                    size_t huge_page_size)
    : shard_block_size_(std::min(kMaxShardBlockSize, block_size / 8)),
      shards_(),
      arena_(block_size, tracker, huge_page_size) {
    Fixup();
}

ConcurrentArena<N>::Shard* ConcurrentArena<N>::Repick() {
    auto shard_and_index = shards_.AccessElementAndIndex();
    // even if we are cpu 0, use a non-zero tls_cpuid so we can tell we
    // have repicked
    tls_cpuid = shard_and_index.second | shards_.Size();
    return shard_and_index.first;
}

}  // namespace utils::memory