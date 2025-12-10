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

ParsedResult parseFrame(const std::vector<uint8_t>& frame_body) {
    if (frame_body.size() < 3) 
        throw std::runtime_error("Too small body to contain priority + topic_len");

    const uint8_t* data = frame_body.data();
    size_t len = frame_body.size();

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
    if (payload_offset < len) 
        r.payload.assign(frame_body.begin() + payload_offset, frame_body.end());
    else 
        r.payload.clear();

    return r;
}

ParsedResult parseTCPFrame(const std::vector<uint8_t>& full_frame_include_length) {
    if (full_frame_include_length.size() < 4) 
        throw std::runtime_error("Too small to contain frame length");

    const uint8_t* data = full_frame_include_length.data();
    uint32_t frame_len = read_uint32_be(data);
    if (frame_len != full_frame_include_length.size() - 4) 
        throw std::runtime_error("Frame length mismatch");

    std::vector<uint8_t> frame_body(full_frame_include_length.begin() + 4, full_frame_include_length.end());
    return parseFrame(frame_body);
}
