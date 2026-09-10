#pragma once

#include <eventstream/platform/platform_contract.hpp>

#include <chrono>
#include <cstdint>

namespace eventstream::platform {

class LinuxTimerBackend {
public:
    using NativeHandle = int;

    LinuxTimerBackend();
    ~LinuxTimerBackend() noexcept;

    LinuxTimerBackend(const LinuxTimerBackend&) = delete;
    LinuxTimerBackend& operator=(const LinuxTimerBackend&) = delete;

    LinuxTimerBackend(LinuxTimerBackend&& other) noexcept;
    LinuxTimerBackend& operator=(LinuxTimerBackend&& other) noexcept;

    Status arm(
        std::chrono::nanoseconds initial,
        std::chrono::nanoseconds period);
    Status disarm() noexcept;
    std::uint64_t wait();

    NativeHandle native_handle() noexcept;

private:
    int fd_{-1};
};

} // namespace eventstream::platform