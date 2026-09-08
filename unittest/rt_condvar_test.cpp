#include <eventstream/rt/rt_condvar.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <time.h>
#include <vector>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(RtCondvarTest, WaitsForPredicateAndWakesOneThread) {
    eventstream::rt::RtMutex mutex;
    eventstream::rt::RtCondvar condition;

    std::atomic<bool> started{false};
    bool ready = false;
    bool completed = false;

    std::thread worker([&] {
        eventstream::rt::RtLockGuard lock(mutex);
        started.store(true, std::memory_order_release);

        while (!ready) {
            condition.wait(mutex);
        }

        completed = true;
    });

    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    {
        eventstream::rt::RtLockGuard lock(mutex);
        ready = true;
    }

    condition.notifyOne();
    worker.join();

    EXPECT_TRUE(completed);
}

TEST(RtCondvarTest, TimedWaitReturnsFalseOnTimeout) {
    eventstream::rt::RtMutex mutex;
    eventstream::rt::RtCondvar condition;
    eventstream::rt::RtLockGuard lock(mutex);

    timespec start{};
    timespec end{};
    ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &start), 0);

    EXPECT_FALSE(condition.waitFor(mutex, 100ms));

    ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &end), 0);

    const auto elapsed =
        std::chrono::seconds(end.tv_sec - start.tv_sec) +
        std::chrono::nanoseconds(end.tv_nsec - start.tv_nsec);

    EXPECT_GE(elapsed, 80ms);
    EXPECT_LT(elapsed, 500ms);
}

TEST(RtCondvarTest, NotifyAllWakesAllWorkers) {
    constexpr int kWorkers = 10;

    eventstream::rt::RtMutex mutex;
    eventstream::rt::RtCondvar condition;

    std::atomic<int> started{0};
    std::atomic<int> completed{0};
    bool shutdown = false;
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);

    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back([&] {
            eventstream::rt::RtLockGuard lock(mutex);
            started.fetch_add(1, std::memory_order_release);

            while (!shutdown) {
                condition.wait(mutex);
            }

            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    while (started.load(std::memory_order_acquire) < kWorkers) {
        std::this_thread::yield();
    }

    {
        eventstream::rt::RtLockGuard lock(mutex);
        shutdown = true;
    }

    condition.notifyAll();

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(completed.load(), kWorkers);
}

TEST(RtCondvarTest, RejectsNegativeTimeout) {
    eventstream::rt::RtMutex mutex;
    eventstream::rt::RtCondvar condition;
    eventstream::rt::RtLockGuard lock(mutex);

    EXPECT_THROW(
        condition.waitFor(
            mutex,
            std::chrono::nanoseconds(-1)),
        std::invalid_argument);
}

TEST(RtConditionTest, BundlesMutexAndConditionVariable) {
    eventstream::rt::RtCondition condition;
    std::atomic<bool> completed{false};
    bool ready = false;

    std::thread worker([&] {
        condition.wait([&] {
            return ready;
        });

        completed.store(true, std::memory_order_release);
    });

    std::this_thread::yield();

    {
        eventstream::rt::RtLockGuard lock(condition.mutex());
        ready = true;
    }

    condition.notifyOne();
    worker.join();

    EXPECT_TRUE(completed.load(std::memory_order_acquire));
}