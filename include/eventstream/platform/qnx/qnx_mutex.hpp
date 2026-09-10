#pragma once

#include <eventstream/platform/platform_contract.hpp>

#include <pthread.h>

namespace eventstream::platform {

class QnxMutexBackend {
public:
    using NativeHandle = pthread_mutex_t*;

    explicit QnxMutexBackend(const MutexOptions& options = {});

    ~QnxMutexBackend() noexcept;

    QnxMutexBackend(const QnxMutexBackend&) = delete;
    QnxMutexBackend& operator=(const QnxMutexBackend&) = delete;

    QnxMutexBackend(QnxMutexBackend&& other) noexcept = delete;
    QnxMutexBackend& operator=(QnxMutexBackend&& other) noexcept = delete;

    void lock();
    void unlock();
    bool try_lock();

    NativeHandle native_handle() noexcept;

private:
    pthread_mutex_t mutex_{};
    bool initialized_{false};
};

} // namespace eventstream::platform