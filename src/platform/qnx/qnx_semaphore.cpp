#include <eventstream/platform/qnx/qnx_semaphore.hpp>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <time.h>

namespace eventstream::platform {
namespace {

[[noreturn]] void throwSemaphoreError(int error, const char* operation) {
    throw std::system_error(error, std::generic_category(), operation);
}

timespec realtimeDeadline(std::chrono::nanoseconds timeout) {
    if (timeout < std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument(
            "QnxSemaphoreBackend timeout cannot be negative");
    }
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        throw std::system_error(
            errno, std::generic_category(), "clock_gettime(CLOCK_REALTIME)");
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto remainder =
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - seconds);
    timespec deadline{};
    deadline.tv_sec = now.tv_sec + seconds.count();
    deadline.tv_nsec = now.tv_nsec + remainder.count();
    if (deadline.tv_nsec >= 1'000'000'000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1'000'000'000L;
    }
    return deadline;
}

} // namespace

QnxSemaphoreBackend::QnxSemaphoreBackend(unsigned initialCount) {
    if (sem_init(&semaphore_, 0, initialCount) != 0) {
        throwSemaphoreError(errno, "sem_init");
    }
    initialized_ = true;
}

QnxSemaphoreBackend::~QnxSemaphoreBackend() noexcept {
    if (initialized_) {
        (void)sem_destroy(&semaphore_);
    }
}

void QnxSemaphoreBackend::wait() {
    for (;;) {
        if (sem_wait(&semaphore_) == 0) {
            return;
        }
        if (errno != EINTR) {
            throwSemaphoreError(errno, "sem_wait");
        }
    }
}

bool QnxSemaphoreBackend::try_wait() {
    if (sem_trywait(&semaphore_) == 0) {
        return true;
    }
    if (errno == EAGAIN) {
        return false;
    }
    throwSemaphoreError(errno, "sem_trywait");
}

bool QnxSemaphoreBackend::wait_for(std::chrono::nanoseconds timeout) {
    const timespec deadline = realtimeDeadline(timeout);
    for (;;) {
        if (sem_timedwait(&semaphore_, &deadline) == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ETIMEDOUT) {
            return false;
        }
        throwSemaphoreError(errno, "sem_timedwait");
    }
}

void QnxSemaphoreBackend::post() {
    if (sem_post(&semaphore_) != 0) {
        throwSemaphoreError(errno, "sem_post");
    }
}

unsigned QnxSemaphoreBackend::value() const {
    int value = 0;
    if (sem_getvalue(const_cast<sem_t*>(&semaphore_), &value) != 0) {
        throwSemaphoreError(errno, "sem_getvalue");
    }
    return value < 0 ? 0U : static_cast<unsigned>(value);
}

QnxSemaphoreBackend::NativeHandle
QnxSemaphoreBackend::native_handle() noexcept {
    return &semaphore_;
}

} // namespace eventstream::platform