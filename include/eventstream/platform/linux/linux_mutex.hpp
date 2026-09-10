#pragma once

#include <eventstream/platform/platform_contract.hpp>

#include <pthread.h>

namespace eventstream::platform {

class LinuxMutexBackend {
public:
    using NativeHandle = pthread_mutex_t*;

    explicit LinuxMutexBackend(const MutexOptions& options = {});
    ~LinuxMutexBackend() noexcept;

    LinuxMutexBackend(const LinuxMutexBackend&) = delete;
    LinuxMutexBackend& operator=(const LinuxMutexBackend&) = delete;
    LinuxMutexBackend(LinuxMutexBackend&&) = delete;
    LinuxMutexBackend& operator=(LinuxMutexBackend&&) = delete;

    void lock();
    bool try_lock();
    void unlock();

    NativeHandle native_handle() noexcept;

private:
    pthread_mutex_t mutex_{};
    bool initialized_{false};
};

} // namespace eventstream::platform