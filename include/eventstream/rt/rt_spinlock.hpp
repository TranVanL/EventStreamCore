#pragma once

#include <atomic>

namespace eventstream::rt {

class RtSpinlock {
public:
    RtSpinlock() noexcept = default;
    ~RtSpinlock() noexcept = default;

    void lock() noexcept;
    bool tryLock() noexcept;
    void unlock() noexcept;

    RtSpinlock(const RtSpinlock&) = delete;
    RtSpinlock& operator=(const RtSpinlock&) = delete;
    RtSpinlock(RtSpinlock&&) = delete;
    RtSpinlock& operator=(RtSpinlock&&) = delete;

private:
    static void pauseOrYield(unsigned attempt) noexcept;

    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class RtSpinlockGuard {
public:
    explicit RtSpinlockGuard(RtSpinlock& lock) noexcept
        : lock_(lock) {
        lock_.lock();
    }

    ~RtSpinlockGuard() noexcept {
        lock_.unlock();
    }

    RtSpinlockGuard(const RtSpinlockGuard&) = delete;
    RtSpinlockGuard& operator=(const RtSpinlockGuard&) = delete;

private:
    RtSpinlock& lock_;
};

} // namespace eventstream::rt