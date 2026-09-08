#include <eventstream/rt/rt_spinlock.hpp>

#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

TEST(RtSpinlockTest, TryLockReportsBusyState) {
    eventstream::rt::RtSpinlock lock;

    EXPECT_TRUE(lock.tryLock());
    EXPECT_FALSE(lock.tryLock());

    lock.unlock();

    EXPECT_TRUE(lock.tryLock());
    lock.unlock();
}

TEST(RtSpinlockTest, ProtectsSharedCounter) {
    eventstream::rt::RtSpinlock lock;
    std::uint64_t counter = 0;

    constexpr int kThreads = 8;
    constexpr int kIterations = 1000000;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            for (int j = 0; j < kIterations; ++j) {
                eventstream::rt::RtSpinlockGuard guard(lock);
                ++counter;
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(
        counter,
        static_cast<std::uint64_t>(kThreads) * kIterations);
}