#include <eventstream/platform/linux/linux_channel.hpp>

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace eventstream::platform {
namespace {

Status statusFromErrno(int error) noexcept {
    switch (error) {
    case EAGAIN:
        return {ErrorCode::WouldBlock, error};
    case EINTR:
        return {ErrorCode::Interrupted, error};
    case ETIMEDOUT:
        return {ErrorCode::Timeout, error};
    case EBADF:
        return {ErrorCode::Closed, error};
    case EINVAL:
    case EMSGSIZE:
        return {ErrorCode::InvalidArgument, error};
    default:
        return {ErrorCode::NativeError, error};
    }
}

} // namespace

LinuxChannelBackend::LinuxChannelBackend(
    const ChannelOptions& options) {
    if (options.endpoint == nullptr ||
        options.endpoint[0] != '/') {
        throw std::invalid_argument(
            "POSIX message queue endpoint must start with '/'");
    }

    if (options.create &&
        (options.maxMessages == 0 || options.messageSize == 0)) {
        throw std::invalid_argument(
            "created message queue requires capacity and message size");
    }

    mq_attr attributes{};
    attributes.mq_maxmsg =
        static_cast<long>(options.maxMessages);
    attributes.mq_msgsize =
        static_cast<long>(options.messageSize);

    int flags = O_RDWR;
    if (options.create) {
        flags |= O_CREAT;
    }

    queue_ = mq_open(
        options.endpoint,
        flags,
        permissions_,
        options.create ? &attributes : nullptr);
    if (queue_ == static_cast<mqd_t>(-1)) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "mq_open");
    }

    mq_attr actual{};
    if (mq_getattr(queue_, &actual) != 0 || actual.mq_msgsize <= 0) {
        const int error = errno != 0 ? errno : EINVAL;
        (void)mq_close(queue_);
        queue_ = static_cast<mqd_t>(-1);
        throw std::system_error(
            error,
            std::generic_category(),
            "mq_getattr");
    }

    messageSize_ = static_cast<std::size_t>(actual.mq_msgsize);
    closed_ = false;
}

LinuxChannelBackend::~LinuxChannelBackend() noexcept {
    (void)close();
}

LinuxChannelBackend::LinuxChannelBackend(
    LinuxChannelBackend&& other) noexcept
    : queue_(other.queue_),
      messageSize_(other.messageSize_),
      closed_(other.closed_) {
    other.queue_ = static_cast<mqd_t>(-1);
    other.messageSize_ = 0;
    other.closed_ = true;
}

LinuxChannelBackend& LinuxChannelBackend::operator=(
    LinuxChannelBackend&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    (void)close();
    queue_ = other.queue_;
    messageSize_ = other.messageSize_;
    closed_ = other.closed_;
    other.queue_ = static_cast<mqd_t>(-1);
    other.messageSize_ = 0;
    other.closed_ = true;
    return *this;
}

Status LinuxChannelBackend::send(const ByteView& message) {
    if (closed_) {
        return {ErrorCode::Closed, EBADF};
    }
    if ((message.data == nullptr && message.size != 0) ||
        message.size > messageSize_) {
        return {ErrorCode::InvalidArgument, EMSGSIZE};
    }

    if (mq_send(
            queue_,
            reinterpret_cast<const char*>(message.data),
            message.size,
            message.priority) != 0) {
        return statusFromErrno(errno);
    }
    return {};
}

ReceiveResult LinuxChannelBackend::receive(
    const MutableByteView& buffer) {
    if (closed_) {
        return {{ErrorCode::Closed, EBADF}, 0, 0};
    }
    if ((buffer.data == nullptr && buffer.capacity != 0) ||
        buffer.capacity < messageSize_) {
        return {{ErrorCode::InvalidArgument, EMSGSIZE}, 0, 0};
    }

    unsigned priority = 0;
    const ssize_t received = mq_receive(
        queue_,
        reinterpret_cast<char*>(buffer.data),
        buffer.capacity,
        &priority);
    if (received < 0) {
        return {statusFromErrno(errno), 0, 0};
    }

    return {{}, static_cast<std::size_t>(received), priority};
}

Status LinuxChannelBackend::close() noexcept {
    if (closed_) {
        return {};
    }

    const int result = mq_close(queue_);
    const int error = errno;
    queue_ = static_cast<mqd_t>(-1);
    messageSize_ = 0;
    closed_ = true;

    if (result != 0) {
        return statusFromErrno(error);
    }
    return {};
}

LinuxChannelBackend::NativeHandle
LinuxChannelBackend::native_handle() noexcept {
    return queue_;
}

} // namespace eventstream::platform