#include "ingest/tcp_parser.hpp"
#include "event/EventFactory.hpp"
#include <stdexcept>

EventStream::ParsedResult EventStream::parseFrame(const std::vector<uint8_t>& raw) {
    if (raw.size() < 2) {
        throw std::runtime_error("Frame too small");
    }

    uint16_t topic_len = (raw[0] << 8) | raw[1];
    if (raw.size() < 2 + topic_len) {
        throw std::runtime_error("Invalid topic length");
    }

    ParsedResult r;
    r.topic = std::string(raw.begin() + 2, raw.begin() + 2 + topic_len);
    r.payload = std::vector<uint8_t>(raw.begin() + 2 + topic_len, raw.end());

    return r;
}

EventStream::Event EventStream::parseTCP(const std::vector<uint8_t>& raw) {
    ParsedResult parsed = parseFrame(raw);

    return EventStream::EventFactory::createEvent(
        EventSourceType::TCP,
        parsed.payload,
        parsed.topic,
        {{"topic", parsed.topic}},
        false
    );
}