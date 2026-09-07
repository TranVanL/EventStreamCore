#include <eventstream/rt/rt_semaphore.hpp>

#include <atomic>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <unistd.h>

TEST(RtSemaphoreTest, WaitPostAndTryWait) {
    eventstream::rt::RtSemaphore semaphore(1);

    EXPECT_EQ(semaphore.getValue(), 1);
    semaphore.wait();
    EXPECT_FALSE(semaphore.tryWait());

    semaphore.post();
    EXPECT_TRUE(semaphore.tryWait());
}

TEST(RtSemaphoreTest, WaitBlocksUntilPost) {
    eventstream::rt::RtSemaphore semaphore(0);
    std::atomic<bool> ready{false};
    std::atomic<bool> completed{false};

    std::thread worker([&] {
        ready.store(true, std::memory_order_release);
        semaphore.wait();
        completed.store(true, std::memory_order_release);
    });

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    EXPECT_FALSE(completed.load(std::memory_order_acquire));

    semaphore.post();
    worker.join();

    EXPECT_TRUE(completed.load(std::memory_order_acquire));
}

TEST(RtSemaphoreTest, NamedSemaphoreRoundTrip) {
    const std::string name =
        "/eventstream_rt_sem_" + std::to_string(getpid());

    // Remove a stale object from an earlier interrupted test run.
    sem_unlink(name.c_str());

    eventstream::rt::RtSemaphore owner(
        name.c_str(),
        1,
        true);

    eventstream::rt::RtSemaphore peer(
        name.c_str(),
        0,
        false);

    EXPECT_TRUE(owner.tryWait());
    EXPECT_FALSE(peer.tryWait());

    owner.post();
    EXPECT_TRUE(peer.tryWait());
}