#include <gtest/gtest.h>
#include "event/EventFactory.hpp"

TEST(EventFactory , creatEvent) {
    using namespace EventStream;
    
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    std::unordered_map<std::string, std::string> metadata = {{"key1", "value1"}, {"key2", "value2"}};

    // include routing_key in metadata; topic is a separate argument
    metadata["routing_key"] = "route1";
    std::string topic = "topic1";
    Event event = EventFactory::createEvent(EventSourceType::UDP, EventPriority::MEDIUM,
                                             std::move(payload), std::move(topic), std::move(metadata));
    EXPECT_EQ(event.header.sourceType, EventSourceType::UDP);
    EXPECT_EQ(event.body.size(), 3);
    EXPECT_EQ(event.topic, "topic1");

}

