#pragma once

#include <chrono>
#include <pthread.h>

#include <eventstream/rt/rt_mutex.hpp>

namespace eventstream::rt {

class RtCondvar {
public:
    RtCondvar();
    ~RtCondvar() noexcept;

    RtCondvar(const RtCondvar&) = delete;
    RtCondvar& operator=(const RtCondvar&) = delete;
    RtCondvar(RtCondvar&&) = delete;
    RtCondvar& operator=(RtCondvar&&) = delete;

    // The caller must hold mutex. The mutex is released atomically while
    // waiting and reacquired before this function returns.
    void wait(RtMutex& mutex);

    // Returns false only on timeout. A successful wakeup does not prove that
    // the caller's predicate is true; callers must check it in a while loop.
    bool waitFor(
        RtMutex& mutex,
        std::chrono::nanoseconds timeout);

    void notifyOne();
    void notifyAll();

private:
    pthread_cond_t cond_{};
    bool is_initialized_{false};
};

using RtCondVar = RtCondvar;

class RtCondition {
private:
    RtMutex mutex_;
    RtCondvar cond_;

public:
    RtCondition() = default;
    ~RtCondition() noexcept = default;

    RtCondition(const RtCondition&) = delete;
    RtCondition& operator=(const RtCondition&) = delete;
    RtCondition(RtCondition&&) = delete;
    RtCondition& operator=(RtCondition&&) = delete;

    RtMutex& mutex() noexcept {
        return mutex_;
    }

    RtCondvar& condvar() noexcept {
        return cond_;
    }

    RtMutex& getMutex() noexcept {
        return mutex();
    }

    RtCondvar& getCondVar() noexcept {
        return condvar();
    }

    void notifyOne() {
        cond_.notifyOne();
    }

    void notifyAll() {
        cond_.notifyAll();
    }

    template <typename Predicate>
    void wait(Predicate predicate) {
        RtLockGuard lock(mutex_);
        while (!predicate()) {
            cond_.wait(mutex_);
        }
    }
};

} // namespace eventstream::rt

