#pragma once

#include <eventstream/platform/platform_contract.hpp>

#include <chrono>
#include <semaphore.h>

namespace eventstream::platform {

class QnxSemaphoreBackend {
public:
    using NativeHandle = sem_t*;

    explicit QnxSemaphoreBackend(unsigned initialCount);
    ~QnxSemaphoreBackend() noexcept;

    QnxSemaphoreBackend(const QnxSemaphoreBackend&) = delete;
    QnxSemaphoreBackend& operator=(const QnxSemaphoreBackend&) = delete;
    QnxSemaphoreBackend(QnxSemaphoreBackend&&) = delete;
    QnxSemaphoreBackend& operator=(QnxSemaphoreBackend&&) = delete;

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