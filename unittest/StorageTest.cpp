#include <gtest/gtest.h>
#include "storage_engine/storage_engine.hpp"
#include "event/EventFactory.hpp"

TEST(StorageEngine , storeEvent) {
    using namespace EventStream;

    // Create a temporary storage file
    std::string tempStoragePath = "temp_storage.bin";
    {
        StorageEngine storageEngine(tempStoragePath);

        // Create a sample event
        std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40};
        std::unordered_map<std::string, std::string> metadata = {{"meta1", "data1"}};
        Event event = EventFactory::createEvent(EventSourceType::TCP, payload, "test_topic", metadata);

        // Store the event
        EXPECT_NO_THROW(storageEngine.storeEvent(event));
    }

    // Clean up temporary storage file
    std::remove(tempStoragePath.c_str());
}