#pragma once
#include <eventstream/rt/rt_policy.hpp>

#include <functional>
#include <pthread.h>

namespace eventstream::platform {
class QnxThreadBackend {
public: 
    using NativeHandle = pthread_t;
    using Entry = std::function<void()>;

    QnxThreadBackend() = default;
    ~QnxThreadBackend() noexcept;

    QnxThreadBackend(const QnxThreadBackend&) = delete;
    QnxThreadBackend& operator=(const QnxThreadBackend&) = delete;

    QnxThreadBackend(QnxThreadBackend&&) noexcept;
    QnxThreadBackend& operator=(QnxThreadBackend&&) noexcept;

    static QnxThreadBackend create(Entry entry);

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