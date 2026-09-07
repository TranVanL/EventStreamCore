#include <eventstream/rt/rt_spinlock.hpp>

#include <sched.h>
#include <thread>

namespace eventstream::rt {

void RtSpinlock::lock() noexcept {
    unsigned attempt = 0;

    while (flag_.test_and_set(std::memory_order_acquire)) {
        pauseOrYield(attempt++);
    }
}

bool RtSpinlock::tryLock() noexcept {
    return !flag_.test_and_set(std::memory_order_acquire);
}

void RtSpinlock::unlock() noexcept {
    flag_.clear(std::memory_order_release);
}

void RtSpinlock::pauseOrYield(unsigned attempt) noexcept {
    if (attempt < 100) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        asm volatile("yield" ::: "memory");
#else
        std::this_thread::yield();
#endif
        return;
    }

    (void)sched_yield();
}

} // namespace eventstream::rt