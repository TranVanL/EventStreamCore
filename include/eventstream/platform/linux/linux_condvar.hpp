#pragma once

#include <eventstream/platform/platform_contract.hpp>
#include <eventstream/platform/linux/linux_mutex.hpp>

#include <chrono>
#include <pthread.h>

namespace eventstream::platform {

class LinuxCondvarBackend {
public:
    using NativeHandle = pthread_cond_t*;

    LinuxCondvarBackend();
    ~LinuxCondvarBackend() noexcept;

    LinuxCondvarBackend(const LinuxCondvarBackend&) = delete;
    LinuxCondvarBackend& operator=(const LinuxCondvarBackend&) = delete;
    LinuxCondvarBackend(LinuxCondvarBackend&&) = delete;
    LinuxCondvarBackend& operator=(LinuxCondvarBackend&&) = delete;

    void wait(LinuxMutexBackend& mutex);
    bool wait_for(
        LinuxMutexBackend& mutex,
        std::chrono::nanoseconds timeout);

    void notify_one() noexcept;
    void notify_all() noexcept;

    NativeHandle native_handle() noexcept;

private:
    pthread_cond_t cond_{};
    bool initialized_{false};
};

} // namespace eventstream::platform