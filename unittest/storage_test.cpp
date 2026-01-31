// ============================================================================
// STORAGE ENGINE UNIT TESTS
// ============================================================================
// Tests for binary event persistence and DLQ operations
// ============================================================================

#include <gtest/gtest.h>
#include <eventstream/core/storage/storage_engine.hpp>
#include <eventstream/core/events/event_factory.hpp>
#include <filesystem>

using namespace EventStream;

// ============================================================================
// BASIC STORAGE TESTS
// ============================================================================

TEST(StorageEngine, StoreEvent) {
    std::string tempStoragePath = "unittest/temp_storage.bin";
    {
        StorageEngine storageEngine(tempStoragePath);

        // Create a sample event
        std::vector<uint8_t> payload = {0x10, 0x20, 0x30, 0x40};
        std::unordered_map<std::string, std::string> metadata = {{"meta1", "data1"}};
        Event event = EventFactory::createEvent(
            EventSourceType::TCP,
            EventPriority::MEDIUM, 
            std::move(payload), 
            "test_topic", 
            std::move(metadata)
        );

        // Store the event
        storageEngine.storeEvent(event);
    }

    // Verify file was created
    EXPECT_TRUE(std::filesystem::exists(tempStoragePath));
    
    // Clean up temporary storage file
    std::filesystem::remove(tempStoragePath);
}

TEST(StorageEngine, StoreMultipleEvents) {
    std::string tempStoragePath = "unittest/temp_multi_storage.bin";
    {
        StorageEngine storageEngine(tempStoragePath);

        for (int i = 0; i < 10; ++i) {
            std::vector<uint8_t> payload = {static_cast<uint8_t>(i)};
            std::unordered_map<std::string, std::string> metadata = {};
            Event event = EventFactory::createEvent(
                EventSourceType::UDP,
                EventPriority::LOW, 
                std::move(payload), 
                "topic_" + std::to_string(i), 
                std::move(metadata)
            );
            storageEngine.storeEvent(event);
        }
    }

    // Verify file was created and has content
    EXPECT_TRUE(std::filesystem::exists(tempStoragePath));
    EXPECT_GT(std::filesystem::file_size(tempStoragePath), 0);
    
    std::filesystem::remove(tempStoragePath);
}

TEST(StorageEngine, ExplicitFlush) {
    std::string tempStoragePath = "unittest/temp_flush_storage.bin";
    {
        StorageEngine storageEngine(tempStoragePath);

        std::vector<uint8_t> payload = {0xAB, 0xCD};
        std::unordered_map<std::string, std::string> metadata = {};
        Event event = EventFactory::createEvent(
            EventSourceType::INTERNAL,
            EventPriority::HIGH, 
            std::move(payload), 
            "flush_test", 
            std::move(metadata)
        );
        
        storageEngine.storeEvent(event);
        storageEngine.flush();  // Explicit flush
    }

    EXPECT_TRUE(std::filesystem::exists(tempStoragePath));
    std::filesystem::remove(tempStoragePath);
}

// ============================================================================
// DLQ (DEAD LETTER QUEUE) TESTS
// ============================================================================

TEST(StorageEngine, AppendDLQEvents) {
    std::string storagePath = "unittest/temp_dlq_storage.bin";
    std::string dlqPath = "unittest/temp_dlq.log";
    {
        StorageEngine storageEngine(storagePath, dlqPath);

        // Create dropped events
        std::vector<EventPtr> dropped_events;
        for (int i = 0; i < 5; ++i) {
            std::vector<uint8_t> payload = {static_cast<uint8_t>(i)};
            std::unordered_map<std::string, std::string> metadata = {};
            auto event = std::make_shared<Event>(EventFactory::createEvent(
                EventSourceType::TCP,
                EventPriority::MEDIUM, 
                std::move(payload), 
                "dropped_topic", 
                std::move(metadata)
            ));
            dropped_events.push_back(event);
        }

        // Append to DLQ
        storageEngine.appendDLQ(dropped_events, "Backpressure CRITICAL");
        
        // Check DLQ stats
        auto stats = storageEngine.getDLQStats();
        EXPECT_EQ(stats.total_dropped, 5);
        EXPECT_EQ(stats.last_drop_reason, "Backpressure CRITICAL");
    }

    // Verify DLQ file was created
    EXPECT_TRUE(std::filesystem::exists(dlqPath));
    
    std::filesystem::remove(storagePath);
    std::filesystem::remove(dlqPath);
}

TEST(StorageEngine, DLQPathDerivedFromStorage) {
    // When DLQ path is empty, it should be derived from storage path
    std::string storagePath = "unittest/derived_storage.bin";
    {
        StorageEngine storageEngine(storagePath);  // No DLQ path specified
        
        std::vector<EventPtr> dropped;
        auto event = std::make_shared<Event>();
        event->header.id = 123;
        dropped.push_back(event);
        
        storageEngine.appendDLQ(dropped, "Test drop");
    }

    // DLQ path should be derived as "unittest/derived_dlq_log.txt"
    std::string expectedDlqPath = "unittest/derived_dlq_log.txt";
    
    std::filesystem::remove(storagePath);
    std::filesystem::remove(expectedDlqPath);
}