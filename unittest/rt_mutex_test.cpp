#include <eventstream/rt/rt_mutex.hpp>
#include <eventstream/rt/rt_policy.hpp>
#include <eventstream/rt/rt_thread.hpp>

#include <atomic>
#include <chrono>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace {

constexpr auto kTimeout = 2s;

bool waitFor(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

int firstAllowedCpu() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if (pthread_getaffinity_np(
            pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        return -1;
    }

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &cpuset)) {
            return cpu;
        }
    }

    return -1;
}

eventstream::rt::RtPolicy fifoPolicy(int priority, int cpu) {
    return eventstream::rt::RtPolicyBuilder()
        .fifo()
        .priority(priority)
        .cpus({cpu})
        .build();
}

} // namespace

TEST(RtMutexTest, BasicLockUnlock) {
    eventstream::rt::RtMutex mutex;

    EXPECT_TRUE(mutex.try_lock());
    EXPECT_FALSE(mutex.try_lock());

    mutex.unlock();

    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST(RtMutexTest, LockGuardReleasesMutex) {
    eventstream::rt::RtMutex mutex;

    {
        eventstream::rt::RtLockGuard guard(mutex);
        EXPECT_FALSE(mutex.try_lock());
    }

    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST(RtMutexTest, TimedLockReturnsFalseWhileOwned) {
    eventstream::rt::RtMutex mutex;
    mutex.lock();

    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(mutex.try_lock_for(50ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    mutex.unlock();

    EXPECT_GE(elapsed, 25ms);
}

TEST(RtMutexTest, RobustRecoveryAfterOwnerCancellation) {
    eventstream::rt::RtMutex mutex;
    std::atomic<bool> locked{false};

    std::thread owner([&] {
        mutex.lock();
        locked.store(true, std::memory_order_release);

        // pthread_testcancel() is a cancellation point. The thread exits
        // while owning the robust mutex, simulating owner failure.
        for (;;) {
            pthread_testcancel();
        }
    });

    if (!waitFor(locked)) {
        pthread_cancel(owner.native_handle());
        owner.join();
        FAIL() << "Owner thread did not acquire the mutex";
    }

    ASSERT_EQ(
        pthread_cancel(owner.native_handle()),
        0);
    owner.join();

    // RtMutex::lock() must consume EOWNERDEAD and call
    // pthread_mutex_consistent() internally.
    EXPECT_NO_THROW(mutex.lock());
    EXPECT_NO_THROW(mutex.unlock());

    // The mutex must remain usable after recovery.
    EXPECT_TRUE(mutex.try_lock());
    mutex.unlock();
}

TEST(RtMutexTest, PriorityInheritanceContentionSmoke) {
    const int cpu = firstAllowedCpu();
    ASSERT_GE(cpu, 0);

    eventstream::rt::RtMutex mutex;

    std::atomic<bool> lowReady{false};
    std::atomic<bool> mediumReady{false};
    std::atomic<bool> highReady{false};
    std::atomic<bool> begin{false};
    std::atomic<bool> highWaiting{false};
    std::atomic<bool> highAcquired{false};
    std::atomic<bool> stopMedium{false};
    std::atomic<bool> allPoliciesApplied{true};

    std::chrono::steady_clock::time_point highStart{};
    std::chrono::steady_clock::time_point highEnd{};

    std::thread low([&] {
        if (!eventstream::rt::RtThread::applyToSelf(
                fifoPolicy(10, cpu))) {
            allPoliciesApplied.store(false, std::memory_order_release);
        }
        lowReady.store(true, std::memory_order_release);

        while (!begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        mutex.lock();

        while (!highWaiting.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Simulate bounded work while holding the mutex.
        const auto deadline =
            std::chrono::steady_clock::now() + 100ms;
        while (std::chrono::steady_clock::now() < deadline) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }

        mutex.unlock();
    });

    std::thread medium([&] {
        if (!eventstream::rt::RtThread::applyToSelf(
                fifoPolicy(50, cpu))) {
            allPoliciesApplied.store(false, std::memory_order_release);
        }
        mediumReady.store(true, std::memory_order_release);

        while (!begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const auto deadline =
            std::chrono::steady_clock::now() + 200ms;
        while (!stopMedium.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    });

    std::thread high([&] {
        if (!eventstream::rt::RtThread::applyToSelf(
                fifoPolicy(90, cpu))) {
            allPoliciesApplied.store(false, std::memory_order_release);
        }
        highReady.store(true, std::memory_order_release);

        while (!begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        highStart = std::chrono::steady_clock::now();
        highWaiting.store(true, std::memory_order_release);
        mutex.lock();
        highEnd = std::chrono::steady_clock::now();
        highAcquired.store(true, std::memory_order_release);
        mutex.unlock();
    });

    const bool lowStarted = waitFor(lowReady);
    const bool mediumStarted = waitFor(mediumReady);
    const bool highStarted = waitFor(highReady);

    begin.store(true, std::memory_order_release);

    const bool acquired = waitFor(highAcquired);
    stopMedium.store(true, std::memory_order_release);

    low.join();
    medium.join();
    high.join();

    ASSERT_TRUE(lowStarted);
    ASSERT_TRUE(mediumStarted);
    ASSERT_TRUE(highStarted);

    if (!allPoliciesApplied.load(std::memory_order_acquire)) {
        GTEST_SKIP()
            << "SCHED_FIFO/CPU affinity unavailable; skipping the "
               "scheduler-dependent PI contention assertion";
    }

    ASSERT_TRUE(acquired)
        << "High-priority waiter did not acquire the PI mutex";

    const auto waitTime = highEnd - highStart;
    EXPECT_LT(waitTime, 500ms);
}
