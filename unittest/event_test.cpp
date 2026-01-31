// ============================================================================
// EVENT & EVENT FACTORY UNIT TESTS
// ============================================================================
// Tests for Event creation, metadata handling, and EventFactory functionality
// ============================================================================

#include <gtest/gtest.h>
#include <eventstream/core/events/event_factory.hpp>

using namespace EventStream;

// ============================================================================
// EVENT FACTORY TESTS
// ============================================================================

TEST(EventFactory, CreateEventWithMetadata) {
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    std::unordered_map<std::string, std::string> metadata = {
        {"key1", "value1"},
        {"key2", "value2"},
        {"routing_key", "route1"}
    };
    std::string topic = "topic1";
    
    Event event = EventFactory::createEvent(
        EventSourceType::UDP,
        EventPriority::MEDIUM,
        std::move(payload),
        std::move(topic),
        std::move(metadata)
    );
    
    EXPECT_EQ(event.header.sourceType, EventSourceType::UDP);
    EXPECT_EQ(event.header.priority, EventPriority::MEDIUM);
    EXPECT_EQ(event.body.size(), 3);
    EXPECT_EQ(event.topic, "topic1");
    EXPECT_NE(event.header.timestamp, 0);  // Timestamp should be set
}

TEST(EventFactory, CreateEventWithDifferentPriorities) {
    std::vector<uint8_t> payload = {0xAB};
    
    // High priority
    Event highEvent = EventFactory::createEvent(
        EventSourceType::TCP,
        EventPriority::HIGH,
        std::vector<uint8_t>(payload),
        "high_topic",
        {}
    );
    EXPECT_EQ(highEvent.header.priority, EventPriority::HIGH);
    
    // Low priority
    Event lowEvent = EventFactory::createEvent(
        EventSourceType::TCP,
        EventPriority::LOW,
        std::vector<uint8_t>(payload),
        "low_topic",
        {}
    );
    EXPECT_EQ(lowEvent.header.priority, EventPriority::LOW);
}

TEST(EventFactory, CreateEventWithDifferentSources) {
    std::vector<uint8_t> payload = {0x00};
    
    auto udpEvent = EventFactory::createEvent(
        EventSourceType::UDP, EventPriority::MEDIUM,
        std::vector<uint8_t>(payload), "udp_topic", {}
    );
    EXPECT_EQ(udpEvent.header.sourceType, EventSourceType::UDP);
    
    auto tcpEvent = EventFactory::createEvent(
        EventSourceType::TCP, EventPriority::MEDIUM,
        std::vector<uint8_t>(payload), "tcp_topic", {}
    );
    EXPECT_EQ(tcpEvent.header.sourceType, EventSourceType::TCP);
}

TEST(EventFactory, CreateEventWithEmptyPayload) {
    std::vector<uint8_t> emptyPayload;
    
    Event event = EventFactory::createEvent(
        EventSourceType::INTERNAL,
        EventPriority::LOW,
        std::move(emptyPayload),
        "empty_payload_topic",
        {}
    );
    
    EXPECT_TRUE(event.body.empty());
    EXPECT_EQ(event.topic, "empty_payload_topic");
}

TEST(EventFactory, CreateEventWithLargePayload) {
    // Create a 1MB payload
    std::vector<uint8_t> largePayload(1024 * 1024, 0xAB);
    size_t expectedSize = largePayload.size();
    
    Event event = EventFactory::createEvent(
        EventSourceType::TCP,
        EventPriority::HIGH,
        std::move(largePayload),
        "large_payload_topic",
        {}
    );
    
    EXPECT_EQ(event.body.size(), expectedSize);
}

