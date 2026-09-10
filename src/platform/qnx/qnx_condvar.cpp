#include <eventstream/platform/qnx/qnx_condvar.hpp>

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <time.h>

namespace eventstream::platform {
namespace {

[[noreturn]] void throwCondvarError(int error, const char* operation) {
    throw std::system_error(error, std::generic_category(), operation);
}

timespec monotonicDeadline(std::chrono::nanoseconds timeout) {
    if (timeout < std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument(
            "QnxCondvarBackend timeout cannot be negative");
    }

    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "clock_gettime(CLOCK_MONOTONIC)");
    }

    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto remainder =
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - seconds);
    using TimeType = decltype(timespec::tv_sec);
    if (seconds.count() >
        std::numeric_limits<TimeType>::max() - now.tv_sec) {
        throw std::overflow_error(
            "condition variable deadline exceeds timespec range");
    }

    timespec deadline{};
    deadline.tv_sec = now.tv_sec + static_cast<TimeType>(seconds.count());
    deadline.tv_nsec = now.tv_nsec + remainder.count();
    if (deadline.tv_nsec >= 1'000'000'000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1'000'000'000L;
    }
    return deadline;
}

void handleWaitResult(QnxMutexBackend& mutex, int error, const char* operation) {
    if (error == 0) {
        return;
    }
#if defined(PTHREAD_MUTEX_ROBUST) && defined(EOWNERDEAD)
    if (error == EOWNERDEAD) {
        const int consistent = pthread_mutex_consistent(mutex.native_handle());
        if (consistent != 0) {
            throwCondvarError(consistent, "pthread_mutex_consistent");
        }
        return;
    }
#endif
    throwCondvarError(error, operation);
}

} // namespace

QnxCondvarBackend::QnxCondvarBackend() {
    pthread_condattr_t attributes{};
    int error = pthread_condattr_init(&attributes);
    if (error != 0) {
        throwCondvarError(error, "pthread_condattr_init");
    }

    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error == 0) {
        error = pthread_cond_init(&cond_, &attributes);
    }
    const int destroyError = pthread_condattr_destroy(&attributes);
    if (error != 0) {
        throwCondvarError(error, "pthread_cond initialization");
    }
    if (destroyError != 0) {
        (void)pthread_cond_destroy(&cond_);
        throwCondvarError(destroyError, "pthread_condattr_destroy");
    }
    initialized_ = true;
}

QnxCondvarBackend::~QnxCondvarBackend() noexcept {
    if (initialized_) {
        (void)pthread_cond_destroy(&cond_);
    }
}

void QnxCondvarBackend::wait(QnxMutexBackend& mutex) {
    handleWaitResult(
        mutex,
        pthread_cond_wait(&cond_, mutex.native_handle()),
        "pthread_cond_wait");
}

bool QnxCondvarBackend::wait_for(
    QnxMutexBackend& mutex,
    std::chrono::nanoseconds timeout) {
    const timespec deadline = monotonicDeadline(timeout);
    const int error = pthread_cond_timedwait(
        &cond_, mutex.native_handle(), &deadline);
    if (error == ETIMEDOUT) {
        return false;
    }
    handleWaitResult(mutex, error, "pthread_cond_timedwait");
    return true;
}

void QnxCondvarBackend::notify_one() noexcept {
    (void)pthread_cond_signal(&cond_);
}

void QnxCondvarBackend::notify_all() noexcept {
    (void)pthread_cond_broadcast(&cond_);
}

QnxCondvarBackend::NativeHandle QnxCondvarBackend::native_handle() noexcept {
    return &cond_;
}

} // namespace eventstream::platform