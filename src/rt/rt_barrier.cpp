#include <eventstream/rt/rt_barrier.hpp>

#include <stdexcept>
#include <system_error>

namespace eventstream::rt {

RtBarrier::RtBarrier(unsigned participantCount) {
    if (participantCount == 0) {
        throw std::invalid_argument(
            "barrier participant count must be greater than zero");
    }

    const int result = pthread_barrier_init(
        &barrier_,
        nullptr,
        participantCount);

    if (result != 0) {
        throw std::system_error(
            result,
            std::generic_category(),
            "pthread_barrier_init failed");
    }

    initialized_ = true;
}

RtBarrier::~RtBarrier() noexcept {
    if (!initialized_) {
        return;
    }

    (void)pthread_barrier_destroy(&barrier_);
    initialized_ = false;
}

BarrierWaitResult RtBarrier::wait() {
    const int result = pthread_barrier_wait(&barrier_);

    if (result == 0) {
        return BarrierWaitResult::Participant;
    }

    if (result == PTHREAD_BARRIER_SERIAL_THREAD) {
        return BarrierWaitResult::SerialThread;
    }

    throw std::system_error(
        result,
        std::generic_category(),
        "pthread_barrier_wait failed");
}

} // namespace eventstream::rt