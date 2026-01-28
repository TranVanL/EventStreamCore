#include "ingest/frame_parser.hpp"
#include "event/EventFactory.hpp"
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif


namespace {

inline uint32_t readUint32BE(const uint8_t* data) {
    uint32_t v;
    std::memcpy(&v, data, sizeof(v));
    return ntohl(v);
}

inline uint16_t readUint16BE(const uint8_t* data) {
    uint16_t v;
    std::memcpy(&v, data, sizeof(v));
    return ntohs(v);
}

inline uint8_t readUint8(const uint8_t* data) {
    return *data;
}

} // anonymous namespace

ParsedFrame parseFrameBody(const uint8_t* data, size_t len) {
    if (len < 3)
        throw std::runtime_error("Frame too small: missing priority or topic_len");

    // Priority: 1 byte
    uint8_t priorityVal = readUint8(data);
    if (priorityVal > static_cast<uint8_t>(EventStream::EventPriority::CRITICAL))
        throw std::runtime_error("Invalid priority value");

    // Topic length: 2 bytes (big-endian)
    uint16_t topicLen = readUint16BE(data + 1);
    if (len < 3 + topicLen)
        throw std::runtime_error("Frame too small for declared topic length");

    if (topicLen == 0)
        throw std::runtime_error("Topic length cannot be zero");

    ParsedFrame result;
    result.priority = static_cast<EventStream::EventPriority>(priorityVal);
    result.topic = std::string(reinterpret_cast<const char*>(data + 3), topicLen);

    size_t payloadOffset = 3 + topicLen;
    if (payloadOffset < len) {
        result.payload.assign(data + payloadOffset, data + len);
    }

    return result;
}

ParsedFrame parseFullFrame(const std::vector<uint8_t>& fullFrame) {
    if (fullFrame.size() < 4)
        throw std::runtime_error("Frame too small: missing length prefix");

    const uint8_t* data = fullFrame.data();
    uint32_t frameLen = readUint32BE(data);

    if (frameLen != fullFrame.size() - 4)
        throw std::runtime_error("Frame length mismatch");

    return parseFrameBody(data + 4, frameLen);
}
