#include <eventstream/rt/rt_semaphore.hpp>

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>

namespace eventstream::rt {

RtSemaphore::RtSemaphore(unsigned initialValue) {
    if (sem_init(&unnamedStorage_, 0, initialValue) != 0) {
        const int error = errno;
        throw std::system_error(
            error,
            std::generic_category(),
            "sem_init failed");
    }

    handle_ = &unnamedStorage_;
}

RtSemaphore::RtSemaphore(
    const char* name,
    unsigned initialValue,
    bool unlinkOnDestroy)
    : named_(true),
      unlinkOnDestroy_(unlinkOnDestroy) {
    if (name == nullptr || name[0] == '\0') {
        throw std::invalid_argument(
            "named semaphore requires a non-empty name");
    }

    name_ = name;

    handle_ = sem_open(
        name_.c_str(),
        O_CREAT,
        0644,
        initialValue);

    if (handle_ == SEM_FAILED) {
        handle_ = nullptr;

        const int error = errno;
        throw std::system_error(
            error,
            std::generic_category(),
            "sem_open failed");
    }
}

RtSemaphore::~RtSemaphore() noexcept {
    if (handle_ == nullptr) {
        return;
    }

    if (named_) {
        (void)sem_close(handle_);

        if (unlinkOnDestroy_) {
            (void)sem_unlink(name_.c_str());
        }
    } else {
        (void)sem_destroy(handle_);
    }

    handle_ = nullptr;
}

void RtSemaphore::wait() {
    while (sem_wait(handle_) != 0) {
        const int error = errno;

        if (error == EINTR) {
            continue;
        }

        throw std::system_error(
            error,
            std::generic_category(),
            "sem_wait failed");
    }
}

bool RtSemaphore::tryWait() {
    if (sem_trywait(handle_) == 0) {
        return true;
    }

    const int error = errno;

    if (error == EAGAIN) {
        return false;
    }

    throw std::system_error(
        error,
        std::generic_category(),
        "sem_trywait failed");
}

void RtSemaphore::post() {
    if (sem_post(handle_) != 0) {
        const int error = errno;
        throw std::system_error(
            error,
            std::generic_category(),
            "sem_post failed");
    }
}

int RtSemaphore::getValue() const {
    int value = 0;

    if (sem_getvalue(handle_, &value) != 0) {
        const int error = errno;
        throw std::system_error(
            error,
            std::generic_category(),
            "sem_getvalue failed");
    }

    return value;
}

void RtSemaphore::unlink() {
    if (!named_) {
        return;
    }

    if (sem_unlink(name_.c_str()) != 0) {
        const int error = errno;

        if (error != ENOENT) {
            throw std::system_error(
                error,
                std::generic_category(),
                "sem_unlink failed");
        }
    }

    unlinkOnDestroy_ = false;
}

} // namespace eventstream::rt