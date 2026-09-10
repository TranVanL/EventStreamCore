#pragma once

#include <eventstream/platform/platform_contract.hpp>

#include <chrono>
#include <semaphore.h>

namespace eventstream::platform {

class LinuxSemaphoreBackend {
public:
    using NativeHandle = sem_t*;

    explicit LinuxSemaphoreBackend(unsigned initialCount);
    ~LinuxSemaphoreBackend() noexcept;

    LinuxSemaphoreBackend(const LinuxSemaphoreBackend&) = delete;
    LinuxSemaphoreBackend& operator=(const LinuxSemaphoreBackend&) = delete;
    LinuxSemaphoreBackend(LinuxSemaphoreBackend&&) = delete;
    LinuxSemaphoreBackend& operator=(LinuxSemaphoreBackend&&) = delete;

    void wait();
    bool try_wait();
    bool wait_for(std::chrono::nanoseconds timeout);
    void post();
    unsigned value() const;

    NativeHandle native_handle() noexcept;

private:
    sem_t semaphore_{};
    bool initialized_{false};
};

} // namespace eventstream::platform