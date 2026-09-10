#pragma once

#include <eventstream/platform/platform_contract.hpp>

#include <mqueue.h>

namespace eventstream::platform {

class LinuxChannelBackend {
public:
    using NativeHandle = mqd_t;

    explicit LinuxChannelBackend(const ChannelOptions& options);
    ~LinuxChannelBackend() noexcept;

    LinuxChannelBackend(const LinuxChannelBackend&) = delete;
    LinuxChannelBackend& operator=(const LinuxChannelBackend&) = delete;

    LinuxChannelBackend(LinuxChannelBackend&& other) noexcept;
    LinuxChannelBackend& operator=(LinuxChannelBackend&& other) noexcept;

    Status send(const ByteView& message);
    ReceiveResult receive(const MutableByteView& buffer);
    Status close() noexcept;

    NativeHandle native_handle() noexcept;

private:
    static constexpr mode_t permissions_ = 0644;

    mqd_t queue_{static_cast<mqd_t>(-1)};
    std::size_t messageSize_{0};
    bool closed_{true};
};

} // namespace eventstream::platform