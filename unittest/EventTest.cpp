#include <gtest/gtest.h>
#include "event/EventFactory.hpp"
#include "event/EventBus.hpp"

TEST(EventFactory , creatEvent) {
    using namespace EventStream;
    
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    std::unordered_map<std::string, std::string> metadata = {{"key1", "value1"}, {"key2", "value2"}};

    // include routing_key in metadata; topic is a separate argument
    metadata["routing_key"] = "route1";
    std::string topic = "topic1";
    Event event = EventFactory::createEvent(EventSourceType::UDP, payload, topic, metadata);
    EXPECT_EQ(event.header.sourceType, EventSourceType::UDP);
    EXPECT_EQ(event.body, payload);
    EXPECT_EQ(event.metadata, metadata);
    EXPECT_EQ(event.topic, topic);

}

