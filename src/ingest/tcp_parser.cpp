#include "ingest/tcp_parser.hpp"
#include "event/EventFactory.hpp"
#include <cstring>
#include <stdexcept>


static uint32_t read_uint32_be(const uint8_t* data) {
    uint32_t v;
    std::memcpy(&v, data, sizeof(v));
    return ntohl(v);
}

static uint16_t read_uint16_be(const uint8_t* data) {
    uint16_t v;
    std::memcpy(&v, data, sizeof(v));
    return ntohs(v);
}

static uint8_t read_uint8_be(const uint8_t* data) {
    return *data; 
}

// Day 39 Optimization: Parse from offset without vector allocation
// Accepts pointer + length instead of vector to avoid temporary allocation
ParsedResult parseFrameFromOffset(const uint8_t* data, size_t len) {
    if (len < 3) 
        throw std::runtime_error("Too small body to contain priority + topic_len");

    // Priority: 8 bit 
    uint8_t priority_val = read_uint8_be(data);
    if (priority_val > static_cast<uint8_t>(EventStream::EventPriority::CRITICAL))
        throw std::runtime_error("Invalid priority value");

    // Topic length: 16 bit next
    uint16_t topic_len = read_uint16_be(data + 1);
    if (len < 3 + topic_len) 
        throw std::runtime_error("Frame body too small for declared topic_len");
    
    if (topic_len == 0)
        throw std::runtime_error("Topic length cannot be zero");

    ParsedResult r;
    r.priority = static_cast<EventStream::EventPriority>(priority_val);
    r.topic = std::string(reinterpret_cast<const char*>(data + 3), topic_len);

    size_t payload_offset = 3 + topic_len;
    if (payload_offset < len) {
        r.payload.assign(data + payload_offset, data + len);
    } else {
        r.payload.clear();
    }

    return r;
}

ParsedResult parseFrame(const std::vector<uint8_t>& frame_body) {
    return parseFrameFromOffset(frame_body.data(), frame_body.size());
}

// Day 39 Optimization: Use offset-based parsing instead of vector copy
// Accepts const reference to frame including 4-byte length prefix
ParsedResult parseTCPFrame(const std::vector<uint8_t>& full_frame_include_length) {
    if (full_frame_include_length.size() < 4) 
        throw std::runtime_error("Too small to contain frame length");

    const uint8_t* data = full_frame_include_length.data();
    uint32_t frame_len = read_uint32_be(data);
    if (frame_len != full_frame_include_length.size() - 4) 
        throw std::runtime_error("Frame length mismatch");

    // Day 39: OPTIMIZATION - Parse directly from offset without vector allocation
    // Call parseFrameFromOffset with pointer + length (eliminates temporary vector)
    // Potential gain: 5-8% CPU by avoiding deep copy on every frame parse
    return parseFrameFromOffset(data + 4, frame_len);
}
