#pragma once
#include <eventstream/core/events/event.hpp>
#include <string>
#include <vector>
#include <cstdint>


/**
 * @brief Result of parsing a frame body
 */
struct ParsedFrame {
    EventStream::EventPriority priority;
    std::string topic;
    std::vector<uint8_t> payload;
};

/**
 * @brief Parse frame body (without length prefix)
 * @param data Pointer to frame body start
 * @param len Length of frame body
 * @return ParsedFrame containing priority, topic and payload
 * @throws std::runtime_error if frame is malformed
 */
ParsedFrame parseFrameBody(const uint8_t* data, size_t len);

/**
 * @brief Parse full frame including 4-byte length prefix
 * @param fullFrame Vector containing length prefix + frame body
 * @return ParsedFrame containing priority, topic and payload
 * @throws std::runtime_error if frame is malformed
 */
ParsedFrame parseFullFrame(const std::vector<uint8_t>& fullFrame);


