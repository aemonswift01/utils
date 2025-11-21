#pragma once

#include <atomic>
#include <thread>
#include "port/port_posix.h"
#include "utils/noncopyable.h"

namespace utils::mutex {

class SpinLock : private utils::NonCopyable {
   public:
    bool try_lock() {
        auto currently_locked = locked_.load(std::memory_order_relaxed);
        return !currently_locked &&
               locked_.compare_exchange_weak(currently_locked, true,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }

    /*
    compare_exchange_weak 在失败时会把当前值写回 currently_locked，所以它能重新反映最新的锁状态。
    weak 允许硬件 spuriously fail（虚假失败），但在 try_lock 中不需要重试，因此 weak 比 strong 更快、汇编更简单。
    成功时使用 acquire
        获取锁时必须要保证后续代码看见之前持锁线程所做的修改。
        acquire 与 unlock 中的 release 一起形成 happens-before。
        这是所有互斥锁必须满足的基本规则：
            线程A unlock() (release) —— happens-before —— 线程B lock() (acquire)
    失败时使用relaxed
    在 CAS 失败时：
        try_lock 只是返回 false
        不会执行临界区
        不需要任何同步语义
    */

    void lock() {
        for (size_t tries = 0;; tries++) {
            if (try_lock()) {
                // success
                return;
            }
            port::AsmVolatilePause();
            if (tries > 100) {
                std::this_thread::yield();
            }
        }
    }

    void unlock() { locked_.store(false, std::memory_order_release); }

   private:
    std::atomic<bool> locked_ = false;
};
}  // namespace utils::mutex

/*
SpinLock适合：
    很短操作：几十纳秒级
    无锁结构内部的小临界区
    调度器的就绪队列
    内存池 freelist

为什么不能只用 atomic_flag ？
```
atomic_flag lock = ATOMIC_FLAG_INIT;

void lock() {
    while (lock.test_and_set(std::memory_order_acquire));
}

void unlock() {
    lock.clear(std::memory_order_release);
}
```
但它有几个严重问题：
    没有 pause/yield，会导致 CPU pipeline stall
    在多核下会导致 cache line 暴打（过多的 test_and_set）
    不能控制 backoff，竞争场景性能差

*/