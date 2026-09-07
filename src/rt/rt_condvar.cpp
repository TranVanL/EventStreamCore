#include <eventstream/rt/rt_condvar.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <time.h>

#include <spdlog/spdlog.h>

namespace eventstream::rt {
namespace {

[[noreturn]] void throwCondvarError(
    int errorCode,
    const char* operation) {
    throw std::system_error(
        errorCode,
        std::generic_category(),
        operation);
}

timespec makeMonotonicDeadline(
    std::chrono::nanoseconds timeout) {
    if (timeout < std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument(
            "RtCondvar timeout cannot be negative");
    }

    timespec now{};

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "clock_gettime(CLOCK_MONOTONIC)");
    }

    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            timeout);

    const auto remainder =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timeout - seconds);

    using TimeType = decltype(timespec::tv_sec);

    if (seconds.count() >
        std::numeric_limits<TimeType>::max() -
            now.tv_sec) {
        throw std::overflow_error(
            "RtCondvar deadline exceeds timespec range");
    }

    timespec deadline{};
    deadline.tv_sec =
        now.tv_sec + static_cast<TimeType>(seconds.count());

    deadline.tv_nsec =
        now.tv_nsec + remainder.count();

    if (deadline.tv_nsec >= 1'000'000'000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1'000'000'000L;
    }

    return deadline;
}

void handleRobustMutexResult(
    RtMutex& mutex,
    int result,
    const char* operation) {
    if (result == 0) {
        return;
    }

    if (result == EOWNERDEAD) {
        spdlog::warn(
            "Condition wait reacquired a mutex whose owner died");

        const int consistentResult =
            pthread_mutex_consistent(
                mutex.native_handle());

        if (consistentResult != 0) {
            throwCondvarError(
                consistentResult,
                "pthread_mutex_consistent");
        }

        return;
    }

    if (result == ENOTRECOVERABLE) {
        spdlog::error(
            "Condition wait reacquired a non-recoverable mutex");
    }

    throwCondvarError(result, operation);
}

} // namespace

RtCondvar::RtCondvar() {
    pthread_condattr_t attr{};
    int result = pthread_condattr_init(&attr);
    if (result != 0) {
        spdlog::error(
            "Failed to initialize condition variable attributes: {}",
            std::strerror(result));
        throwCondvarError(result, "pthread_condattr_init");
    }

    result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (result != 0) {
        spdlog::error(
            "Failed to set condition variable clock: {}",
            std::strerror(result));
        pthread_condattr_destroy(&attr);
        throwCondvarError(result, "pthread_condattr_setclock");
    }

    result = pthread_cond_init(&cond_, &attr);
    const int destroyResult = pthread_condattr_destroy(&attr);
    if (result != 0) {
        spdlog::error(
            "Failed to initialize condition variable: {}",
            std::strerror(result));
        throwCondvarError(result, "pthread_cond_init");
    }
    if (destroyResult != 0) {
        spdlog::error(
            "Failed to destroy condition variable attributes: {}",
            std::strerror(destroyResult));
        pthread_cond_destroy(&cond_);
        throwCondvarError(destroyResult, "pthread_condattr_destroy");
    }
    is_initialized_ = true;
}

RtCondvar::~RtCondvar() noexcept {
    if (!is_initialized_) {
        return;
    }
    const int destroyResult = pthread_cond_destroy(&cond_);
    if (destroyResult != 0) {
        spdlog::error(
            "Failed to destroy condition variable: {}",
            std::strerror(destroyResult));
    }
    is_initialized_ = false;
}

void RtCondvar::wait(RtMutex& mutex) {
    const int result = pthread_cond_wait(
        &cond_,
        mutex.native_handle());

    handleRobustMutexResult(mutex, result, "pthread_cond_wait");
}

bool RtCondvar::waitFor(
    RtMutex& mutex,
    std::chrono::nanoseconds timeout) {
    const timespec deadline =
        makeMonotonicDeadline(timeout);

    const int result = pthread_cond_timedwait(
        &cond_,
        mutex.native_handle(),
        &deadline);

    if (result == ETIMEDOUT) {
        return false;
    }

    handleRobustMutexResult(mutex, result, "pthread_cond_timedwait");
    return true;
}

void RtCondvar::notifyOne() {
    const int result = pthread_cond_signal(&cond_);
    if (result != 0) {
        spdlog::error(
            "Failed to signal condition variable: {}",
            std::strerror(result));
        throwCondvarError(result, "pthread_cond_signal");
    }
}

void RtCondvar::notifyAll() {
    const int result = pthread_cond_broadcast(&cond_);
    if (result != 0) {
        spdlog::error(
            "Failed to broadcast condition variable: {}",
            std::strerror(result));
        throwCondvarError(result, "pthread_cond_broadcast");
    }
}

} // namespace eventstream::rt

