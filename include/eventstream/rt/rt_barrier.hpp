#pragma once

#include <pthread.h>

namespace eventstream::rt {

enum class BarrierWaitResult {
    Participant,
    SerialThread
};

class RtBarrier {
public:
    explicit RtBarrier(unsigned participantCount);
    ~RtBarrier() noexcept;

    BarrierWaitResult wait();

    RtBarrier(const RtBarrier&) = delete;
    RtBarrier& operator=(const RtBarrier&) = delete;
    RtBarrier(RtBarrier&&) = delete;
    RtBarrier& operator=(RtBarrier&&) = delete;

private:
    pthread_barrier_t barrier_{};
    bool initialized_{false};
};

} // namespace eventstream::rt