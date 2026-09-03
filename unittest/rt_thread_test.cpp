#include <eventstream/rt/rt_thread.hpp>

#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include <gtest/gtest.h>

namespace {

int firstAllowedCpu() {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);

    if (pthread_getaffinity_np(pthread_self(), sizeof(cpus), &cpus) != 0) {
        return -1;
    }

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &cpus)) {
            return cpu;
        }
    }

    return -1;
}

} // namespace

TEST(RtThreadTest, DescribesPolicy) {
    const auto policy = eventstream::rt::RtPolicyBuilder()
                            .fifo()
                            .priority(80)
                            .cpus({2, 3})
                            .build();

    EXPECT_EQ(eventstream::rt::RtThread::describe(policy),
              "SCHED_FIFO(priority=80, cpus=[2,3])");
}

TEST(RtThreadTest, AppliesBestEffortPolicyAndAffinity) {
    const int cpu = firstAllowedCpu();
    ASSERT_GE(cpu, 0);

    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    std::atomic<bool> affinityApplied{false};

    std::thread worker([&] {
        ready.store(true, std::memory_order_release);

        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        cpu_set_t currentCpus;
        CPU_ZERO(&currentCpus);
        const int result = pthread_getaffinity_np(
            pthread_self(), sizeof(currentCpus), &currentCpus);

        affinityApplied.store(
            result == 0 && CPU_ISSET(cpu, &currentCpus),
            std::memory_order_release);
    });

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto policy = eventstream::rt::RtPolicyBuilder()
                            .other()
                            .cpus({cpu})
                            .build();

    EXPECT_TRUE(eventstream::rt::RtThread::apply(worker, policy));

    release.store(true, std::memory_order_release);
    worker.join();

    EXPECT_TRUE(affinityApplied.load(std::memory_order_acquire));
}

TEST(RtThreadTest, FifoFailureDoesNotStopThread) {
    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    std::atomic<bool> ran{false};

    std::thread worker([&] {
        ready.store(true, std::memory_order_release);

        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        ran.store(true, std::memory_order_release);
    });

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto policy = eventstream::rt::RtPolicyBuilder()
                            .fifo()
                            .priority(10)
                            .build();

    // May be true with CAP_SYS_NICE/root, or false with EPERM on CI.
    static_cast<void>(eventstream::rt::RtThread::apply(worker, policy));

    release.store(true, std::memory_order_release);
    worker.join();

    EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(RtThreadTest, RejectsInvalidPriorityWithoutStoppingThread) {
    std::atomic<bool> ran{false};

    std::thread worker([&] {
        ran.store(true, std::memory_order_release);
    });

    const auto policy = eventstream::rt::RtPolicyBuilder()
                            .fifo()
                            .priority(0)
                            .build();

    EXPECT_FALSE(eventstream::rt::RtThread::apply(worker, policy));
    worker.join();

    EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(RtThreadTest, ApplyToSelfWithBestEffortPolicy) {
    std::atomic<bool> applied{false};

    std::thread worker([&] {
        const auto policy = eventstream::rt::RtPolicyBuilder()
                                .other()
                                .build();
        applied.store(
            eventstream::rt::RtThread::applyToSelf(policy),
            std::memory_order_release);
    });

    worker.join();
    EXPECT_TRUE(applied.load(std::memory_order_acquire));
}