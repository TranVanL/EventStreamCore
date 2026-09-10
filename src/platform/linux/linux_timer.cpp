#include <eventstream/platform/linux/linux_timer.hpp>

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/timerfd.h>
#include <unistd.h>

namespace eventstream::platform {
namespace {

Status invalidArgument() noexcept {
    return {ErrorCode::InvalidArgument, 0};
}

bool toTimespec(
    std::chrono::nanoseconds duration,
    timespec& result) noexcept {
    if (duration < std::chrono::nanoseconds::zero()) {
        return false;
    }

    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto remainder =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            duration - seconds);

    if (seconds.count() >
        std::numeric_limits<time_t>::max()) {
        return false;
    }

    result.tv_sec = static_cast<time_t>(seconds.count());
    result.tv_nsec = static_cast<long>(remainder.count());
    return true;
}

} // namespace

LinuxTimerBackend::LinuxTimerBackend()
    : fd_(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)) {
    if (fd_ < 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "timerfd_create");
    }
}

LinuxTimerBackend::~LinuxTimerBackend() noexcept {
    if (fd_ >= 0) {
        (void)close(fd_);
    }
}

LinuxTimerBackend::LinuxTimerBackend(
    LinuxTimerBackend&& other) noexcept
    : fd_(other.fd_) {
    other.fd_ = -1;
}

LinuxTimerBackend& LinuxTimerBackend::operator=(
    LinuxTimerBackend&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (fd_ >= 0) {
        (void)close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
    return *this;
}

Status LinuxTimerBackend::arm(
    std::chrono::nanoseconds initial,
    std::chrono::nanoseconds period) {
    if (fd_ < 0) {
        return {ErrorCode::Closed, EBADF};
    }

    itimerspec specification{};
    if (!toTimespec(initial, specification.it_value) ||
        !toTimespec(period, specification.it_interval)) {
        return invalidArgument();
    }

    if (timerfd_settime(
            fd_,
            0,
            &specification,
            nullptr) != 0) {
        return {ErrorCode::NativeError, errno};
    }

    return {};
}

Status LinuxTimerBackend::disarm() noexcept {
    if (fd_ < 0) {
        return {ErrorCode::Closed, EBADF};
    }

    const itimerspec specification{};
    if (timerfd_settime(
            fd_,
            0,
            &specification,
            nullptr) != 0) {
        return {ErrorCode::NativeError, errno};
    }
    return {};
}

std::uint64_t LinuxTimerBackend::wait() {
    if (fd_ < 0) {
        throw std::system_error(
            EBADF,
            std::generic_category(),
            "timerfd read on closed timer");
    }

    std::uint64_t expirations = 0;
    for (;;) {
        const ssize_t result = read(
            fd_,
            &expirations,
            sizeof(expirations));

        if (result == static_cast<ssize_t>(sizeof(expirations))) {
            return expirations;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            throw std::system_error(
                errno,
                std::generic_category(),
                "timerfd read");
        }
        throw std::system_error(
            EIO,
            std::generic_category(),
            "short timerfd read");
    }
}

LinuxTimerBackend::NativeHandle
LinuxTimerBackend::native_handle() noexcept {
    return fd_;
}

} // namespace eventstream::platform