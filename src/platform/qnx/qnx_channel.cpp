#include <eventstream/platform/qnx/qnx_channel.hpp>

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/neutrino.h>

namespace eventstream::platform {
namespace {

Status statusFromError(int error) noexcept {
    switch (error) {
    case EAGAIN:
        return {ErrorCode::WouldBlock, error};
    case EINTR:
        return {ErrorCode::Interrupted, error};
    case ETIMEDOUT:
        return {ErrorCode::Timeout, error};
    case EBADF:
    case ESRCH:
        return {ErrorCode::Closed, error};
    case EINVAL:
    case EMSGSIZE:
        return {ErrorCode::InvalidArgument, error};
    default:
        return {ErrorCode::NativeError, error};
    }
}

bool parseEndpoint(
    const char* endpoint,
    pid_t& process,
    int& channel) {
    if (endpoint == nullptr || endpoint[0] == '\0') {
        return false;
    }
    const std::string value(endpoint);
    const std::size_t separator = value.find_last_of(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= value.size()) {
        return false;
    }

    const std::string processText = value.substr(0, separator);
    const std::string channelText = value.substr(separator + 1);
    char* processEnd = nullptr;
    char* channelEnd = nullptr;
    const long processValue = std::strtol(processText.c_str(), &processEnd, 10);
    const long channelValue = std::strtol(channelText.c_str(), &channelEnd, 10);
    if (*processEnd != '\0' || *channelEnd != '\0' ||
        processValue <= 0 || channelValue < 0 ||
        processValue > std::numeric_limits<pid_t>::max() ||
        channelValue > std::numeric_limits<int>::max()) {
        return false;
    }
    process = static_cast<pid_t>(processValue);
    channel = static_cast<int>(channelValue);
    return true;
}

} // namespace

QnxChannelBackend::QnxChannelBackend(const ChannelOptions& options) {
    if (options.create) {
        channel_ = ChannelCreate(0);
        if (channel_ == -1) {
            throw std::system_error(errno, std::generic_category(), "ChannelCreate");
        }
        server_ = true;
        closed_ = false;
        return;
    }

    pid_t process = 0;
    int channel = -1;
    if (!parseEndpoint(options.endpoint, process, channel)) {
        throw std::invalid_argument(
            "QNX channel endpoint must be formatted as pid:chid");
    }
    connection_ = ConnectAttach(
        ND_LOCAL_NODE,
        process,
        channel,
        _NTO_SIDE_CHANNEL,
        0);
    if (connection_ == -1) {
        throw std::system_error(errno, std::generic_category(), "ConnectAttach");
    }
    closed_ = false;
}

QnxChannelBackend::~QnxChannelBackend() noexcept {
    (void)close();
}

QnxChannelBackend::QnxChannelBackend(QnxChannelBackend&& other) noexcept
    : channel_(other.channel_),
      connection_(other.connection_),
      server_(other.server_),
      closed_(other.closed_) {
    other.channel_ = -1;
    other.connection_ = -1;
    other.server_ = false;
    other.closed_ = true;
}

QnxChannelBackend& QnxChannelBackend::operator=(QnxChannelBackend&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    (void)close();
    channel_ = other.channel_;
    connection_ = other.connection_;
    server_ = other.server_;
    closed_ = other.closed_;
    other.channel_ = -1;
    other.connection_ = -1;
    other.server_ = false;
    other.closed_ = true;
    return *this;
}

Status QnxChannelBackend::send(const ByteView& message) {
    if (closed_) {
        return {ErrorCode::Closed, EBADF};
    }
    if (server_) {
        return {ErrorCode::NotSupported, ENOTSUP};
    }
    if (message.data == nullptr && message.size != 0) {
        return {ErrorCode::InvalidArgument, EINVAL};
    }
    if (message.size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {ErrorCode::InvalidArgument, EMSGSIZE};
    }

    const int result = MsgSend(
        connection_,
        message.data,
        static_cast<int>(message.size),
        nullptr,
        0);
    if (result != 0) {
        return statusFromError(errno);
    }
    return {};
}

ReceiveResult QnxChannelBackend::receive(const MutableByteView& buffer) {
    if (closed_) {
        return {{ErrorCode::Closed, EBADF}, 0, 0};
    }
    if (!server_) {
        return {{ErrorCode::NotSupported, ENOTSUP}, 0, 0};
    }
    if (buffer.data == nullptr && buffer.capacity != 0) {
        return {{ErrorCode::InvalidArgument, EINVAL}, 0, 0};
    }
    if (buffer.capacity > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {{ErrorCode::InvalidArgument, EMSGSIZE}, 0, 0};
    }

    const int receiveId = MsgReceive(
        channel_,
        buffer.data,
        static_cast<int>(buffer.capacity),
        nullptr);
    if (receiveId < 0) {
        return {statusFromError(errno), 0, 0};
    }
    if (receiveId == 0) {
        return {{}, 0, 0};
    }

    _msg_info info{};
    if (MsgInfo(receiveId, &info) == -1) {
        const int error = errno;
        (void)MsgReply(receiveId, error, nullptr, 0);
        return {statusFromError(error), 0, 0};
    }
    if (info.msglen < 0 || static_cast<std::size_t>(info.msglen) > buffer.capacity) {
        const int error = EMSGSIZE;
        (void)MsgReply(receiveId, error, nullptr, 0);
        return {{ErrorCode::InvalidArgument, EMSGSIZE}, 0, 0};
    }
    if (MsgReply(receiveId, EOK, nullptr, 0) == -1) {
        return {statusFromError(errno), 0, 0};
    }
    return {{}, static_cast<std::size_t>(info.msglen),
            static_cast<unsigned>(info.priority)};
}

Status QnxChannelBackend::close() noexcept {
    if (closed_) {
        return {};
    }
    const int result = server_
        ? ChannelDestroy(channel_)
        : ConnectDetach(connection_);
    const int error = errno;
    channel_ = -1;
    connection_ = -1;
    server_ = false;
    closed_ = true;
    if (result == -1) {
        return statusFromError(error);
    }
    return {};
}

QnxChannelBackend::NativeHandle QnxChannelBackend::native_handle() noexcept {
    return server_ ? channel_ : connection_;
}

} // namespace eventstream::platform