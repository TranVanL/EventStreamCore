#include <eventstream/rt/rt_barrier.hpp>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

TEST(RtBarrierTest, AllParticipantsReachBarrier) {
    constexpr unsigned kParticipants = 4;

    eventstream::rt::RtBarrier barrier(kParticipants);
    std::atomic<unsigned> arrived{0};
    std::atomic<unsigned> serialThreads{0};
    std::vector<std::thread> workers;

    for (unsigned i = 0; i < kParticipants; ++i) {
        workers.emplace_back([&] {
            arrived.fetch_add(1, std::memory_order_seq_cst);

            if (barrier.wait() ==
                eventstream::rt::BarrierWaitResult::SerialThread) {
                serialThreads.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(arrived.load(), kParticipants);
    EXPECT_EQ(serialThreads.load(), 1U);
}

TEST(RtBarrierTest, BarrierCanBeReusedAcrossPhases) {
    constexpr unsigned kParticipants = 4;
    constexpr unsigned kPhases = 10000;

    eventstream::rt::RtBarrier barrier(kParticipants);
    std::atomic<unsigned> completedPhases{0};
    std::vector<std::thread> workers;

    for (unsigned i = 0; i < kParticipants; ++i) {
        workers.emplace_back([&] {
            for (unsigned phase = 0; phase < kPhases; ++phase) {
                barrier.wait();

                if (phase + 1 == kPhases) {
                    completedPhases.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(completedPhases.load(), kParticipants);
}

TEST(RtBarrierTest, RejectsZeroParticipants) {
    EXPECT_THROW(
        eventstream::rt::RtBarrier(0),
        std::invalid_argument);
}