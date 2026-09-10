#pragma once

#include <eventstream/platform/platform_contract.hpp>

#include <cstddef>

namespace eventstream::platform {

class QnxChannelBackend {
public:
    using NativeHandle = int;

    explicit QnxChannelBackend(const ChannelOptions& options);
    ~QnxChannelBackend() noexcept;

    QnxChannelBackend(const QnxChannelBackend&) = delete;
    QnxChannelBackend& operator=(const QnxChannelBackend&) = delete;

    QnxChannelBackend(QnxChannelBackend&& other) noexcept;
    QnxChannelBackend& operator=(QnxChannelBackend&& other) noexcept;

    Status send(const ByteView& message);
    ReceiveResult receive(const MutableByteView& buffer);
    Status close() noexcept;

    NativeHandle native_handle() noexcept;

private:
    int channel_{-1};
    int connection_{-1};
    bool server_{false};
    bool closed_{true};
};

} // namespace eventstream::platform