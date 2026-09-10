#pragma once

#include <eventstream/rt/rt_policy.hpp>

#include <functional>
#include <pthread.h>

namespace eventstream::platform {

class LinuxThreadBackend {
public:
    using NativeHandle = pthread_t;
    using Entry = std::function<void()>;

    LinuxThreadBackend() noexcept = default;
    ~LinuxThreadBackend() noexcept;

    LinuxThreadBackend(const LinuxThreadBackend&) = delete;
    LinuxThreadBackend& operator=(const LinuxThreadBackend&) = delete;

    LinuxThreadBackend(LinuxThreadBackend&& other) noexcept;
    LinuxThreadBackend& operator=(LinuxThreadBackend&& other) noexcept;

    static LinuxThreadBackend create(Entry entry);

    bool joinable() const noexcept;
    void join();
    void detach();

    bool apply(const eventstream::rt::RtPolicy& policy);
    NativeHandle native_handle() noexcept;

private:
    struct StartContext {
        Entry entry;
    };

    static void* trampoline(void* argument) noexcept;

    pthread_t handle_{};
    bool joinable_{false};
};

} // namespace eventstream::platform