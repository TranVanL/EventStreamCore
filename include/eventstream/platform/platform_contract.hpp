#pragma once

#include <cstddef>
#include <cstdint>

namespace eventstream::platform {

enum class ErrorCode : int {
    None = 0,
    InvalidArgument,
    Timeout,
    WouldBlock,
    Interrupted,
    PermissionDenied,
    ResourceUnavailable,
    OwnerDead,
    Closed,
    NotSupported,
    NativeError
};

struct Status {
    ErrorCode code{ErrorCode::None};
    int nativeCode{0};

    constexpr bool ok() const noexcept {
        return code == ErrorCode::None;
    }

    constexpr explicit operator bool() const noexcept {
        return ok();
    }
};

struct MutexOptions {
    bool priorityInheritance{false};
    bool robust{false};
};

struct ChannelOptions {
    const char* endpoint{nullptr};
    bool create{false};
    std::size_t maxMessages{0};
    std::size_t messageSize{0};
};

struct ByteView {
    const std::uint8_t* data{nullptr};
    std::size_t size{0};
    unsigned priority{0};
};

struct MutableByteView {
    std::uint8_t* data{nullptr};
    std::size_t capacity{0};
};

struct ReceiveResult {
    Status status{};
    std::size_t size{0};
    unsigned priority{0};

    constexpr bool ok() const noexcept {
        return status.ok();
    }
};

} // namespace eventstream::platform