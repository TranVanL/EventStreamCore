#include <eventstream/rt/rt_semaphore.hpp>
#include <eventstream/rt/rt_mutex.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

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

TEST(RtSemaphoreTest, ProducerConsumerBoundedBuffer) {
    constexpr int kCapacity = 100;
    constexpr int kProducerCount = 3;
    constexpr int kConsumerCount = 2;
    constexpr int kItemsPerProducer = 10000;
    constexpr int kExpectedItems =
        kProducerCount * kItemsPerProducer;

    eventstream::rt::RtSemaphore emptySlots(kCapacity);
    eventstream::rt::RtSemaphore filledSlots(0);
    eventstream::rt::RtMutex bufferMutex;
    std::queue<int> buffer;

    std::atomic<int> consumed{0};
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            for (int item = 0; item < kItemsPerProducer; ++item) {
                emptySlots.wait();
                {
                    eventstream::rt::RtLockGuard lock(bufferMutex);
                    buffer.push(producer * kItemsPerProducer + item);
                }
                filledSlots.post();
            }
        });
    }

    for (int consumer = 0; consumer < kConsumerCount; ++consumer) {
        consumers.emplace_back([&] {
            for (;;) {
                filledSlots.wait();

                int value = 0;
                {
                    eventstream::rt::RtLockGuard lock(bufferMutex);
                    value = buffer.front();
                    buffer.pop();
                }

                emptySlots.post();

                // Negative values are termination sentinels.
                if (value < 0) {
                    return;
                }

                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    // Send one poison pill to each consumer after all real items exist.
    for (int i = 0; i < kConsumerCount; ++i) {
        emptySlots.wait();
        {
            eventstream::rt::RtLockGuard lock(bufferMutex);
            buffer.push(-1);
        }
        filledSlots.post();
    }

    for (auto& consumer : consumers) {
        consumer.join();
    }

    EXPECT_EQ(consumed.load(std::memory_order_relaxed), kExpectedItems);
    EXPECT_TRUE(buffer.empty());
}

TEST(RtSemaphoreTest, NamedSemaphoreCrossProcessPost) {
    const std::string name =
        "/eventstream_rt_cross_process_" +
        std::to_string(getpid());

    sem_unlink(name.c_str());

    eventstream::rt::RtSemaphore parentSemaphore(
        name.c_str(),
        0,
        true);

    const pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        try {
            eventstream::rt::RtSemaphore childSemaphore(
                name.c_str(),
                0,
                false);
            childSemaphore.wait();
            _exit(0);
        } catch (...) {
            _exit(1);
        }
    }

    std::this_thread::sleep_for(20ms);
    parentSemaphore.post();

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}