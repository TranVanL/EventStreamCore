#pragma once

#include <eventstream/platform/platform_contract.hpp>

#include <chrono>
#include <cstdint>
#include <pthread.h>
#include <signal.h>
#include <time.h>

namespace eventstream::platform {

class QnxTimerBackend {
public:
    using NativeHandle = timer_t;

    QnxTimerBackend();
    ~QnxTimerBackend() noexcept;

    QnxTimerBackend(const QnxTimerBackend&) = delete;
    QnxTimerBackend& operator=(const QnxTimerBackend&) = delete;

    QnxTimerBackend(QnxTimerBackend&& other) noexcept;
    QnxTimerBackend& operator=(QnxTimerBackend&& other) noexcept;

    Status arm(
        std::chrono::nanoseconds initial,
        std::chrono::nanoseconds period);
    Status disarm() noexcept;
    std::uint64_t wait();

    NativeHandle native_handle() noexcept;

private:
    struct State {
        pthread_mutex_t mutex{};
        pthread_cond_t condition{};
        std::uint64_t expirations{0};
        unsigned callbacks{0};
        bool shuttingDown{false};
        bool initialized{false};
    };

    static void callback(union sigval value) noexcept;
    static void destroyState(State& state) noexcept;

    void reset() noexcept;

    timer_t timer_{};
    State* state_{nullptr};
    bool initialized_{false};
};

} // namespace eventstream::platform