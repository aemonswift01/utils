#pragma once

#include "utils/noncopyable.h"

namespace utils::utils {

class RWMutex : private utils::NonCopyable {
   public:
    void ReadLock();
    void WriteLock();
    void ReadUnlock();
    void WriteUnlock();
};

class ReadLock : private utils::NonCopyable {
   public:
    explicit ReadLock(RWMutex& mutex) : mutex_(mutex) {
        this->mutex_.ReadLock();
    }

    ~ReadLock() { this->mutex_.ReadUnlock(); }

   private:
    RWMutex& const mutex_;
};

class WriteLock : private utils::NonCopyable {
   public:
    explicit WriteLock(RWMutex& mutex) : mutex_(mutex) {
        this->mutex_.WriteLock();
    }

    ~WriteLock() { this->mutex_.WriteUnlock(); }

   private:
    RWMutex& const mutex_;
};

}  // namespace utils::utils