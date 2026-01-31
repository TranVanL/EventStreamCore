// ============================================================================
// EVENT PROCESSOR UNIT TESTS
// ============================================================================
// Tests for ProcessManager lifecycle and event processing
// ============================================================================

#include <gtest/gtest.h>
#include <eventstream/core/events/event_factory.hpp>
#include <eventstream/core/events/event_bus.hpp>
#include <eventstream/core/processor/process_manager.hpp>
#include <eventstream/core/storage/storage_engine.hpp>
#include <thread>

using namespace EventStream;

// ============================================================================
// LIFECYCLE TESTS
// ============================================================================

TEST(EventProcessor, InitWithEventBusOnly) {
    // Test backward compatible constructor (event bus only)
    EventBusMulti eventBus;
    ProcessManager processManager(eventBus);
    // ProcessManager should initialize successfully
    SUCCEED();
}

TEST(EventProcessor, InitWithDependencies) {
    // Test new constructor with Dependencies struct
    EventBusMulti eventBus;
    StorageEngine storageEngine("unittest/test_storage.dat");
    
    ProcessManager::Dependencies deps;
    deps.storage = &storageEngine;
    deps.batch_window = std::chrono::seconds(2);
    
    ProcessManager processManager(eventBus, deps);
    // ProcessManager should initialize with storage wired
    SUCCEED();
    
    std::remove("unittest/test_storage.dat");
}

TEST(EventProcessor, StartStop) {
    EventBusMulti eventBus;
    StorageEngine storageEngine("unittest/test_storage.dat");
    
    ProcessManager::Dependencies deps;
    deps.storage = &storageEngine;
    
    ProcessManager processManager(eventBus, deps);
    
    processManager.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    processManager.stop();
    
    std::remove("unittest/test_storage.dat");
}

// ============================================================================
// EVENT PROCESSING TESTS
// ============================================================================

TEST(EventProcessor, ProcessTransactionalEvent) {
    EventBusMulti eventBus;
    StorageEngine storageEngine("unittest/test_storage.dat");
    
    ProcessManager::Dependencies deps;
    deps.storage = &storageEngine;
    
    ProcessManager processManager(eventBus, deps);
    processManager.start();

    // Create and publish a test event
    std::vector<uint8_t> payload = {0x10, 0x20, 0x30};
    std::unordered_map<std::string, std::string> metadata = {{"key", "value"}};
    Event event = EventFactory::createEvent(
        EventSourceType::TCP, 
        EventPriority::MEDIUM, 
        std::move(payload), 
        "test_topic", 
        std::move(metadata)
    );

    // Push to TRANSACTIONAL queue since priority is MEDIUM
    eventBus.push(EventBusMulti::QueueId::TRANSACTIONAL, std::make_shared<Event>(event));

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    processManager.stop();
    std::remove("unittest/test_storage.dat");
}

TEST(EventProcessor, ProcessRealtimeEvent) {
    EventBusMulti eventBus;
    ProcessManager processManager(eventBus);
    processManager.start();

    // Create HIGH priority event (goes to REALTIME queue)
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    std::unordered_map<std::string, std::string> metadata = {};
    Event event = EventFactory::createEvent(
        EventSourceType::UDP, 
        EventPriority::HIGH,  // HIGH = REALTIME
        std::move(payload), 
        "realtime_topic", 
        std::move(metadata)
    );

    // Push to REALTIME queue
    eventBus.push(EventBusMulti::QueueId::REALTIME, std::make_shared<Event>(event));

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    processManager.stop();
}

TEST(EventProcessor, ProcessBatchEvent) {
    EventBusMulti eventBus;
    StorageEngine storageEngine("unittest/test_batch_storage.dat");
    
    ProcessManager::Dependencies deps;
    deps.storage = &storageEngine;
    deps.batch_window = std::chrono::seconds(1);  // Short batch window for test
    
    ProcessManager processManager(eventBus, deps);
    processManager.start();

    // Create LOW priority event (goes to BATCH queue)
    std::vector<uint8_t> payload = {0x01, 0x02};
    std::unordered_map<std::string, std::string> metadata = {};
    Event event = EventFactory::createEvent(
        EventSourceType::INTERNAL, 
        EventPriority::LOW,  // LOW = BATCH
        std::move(payload), 
        "batch_topic", 
        std::move(metadata)
    );

    // Push to BATCH queue
    eventBus.push(EventBusMulti::QueueId::BATCH, std::make_shared<Event>(event));

    // Wait for batch window + processing
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    processManager.stop();
    std::remove("unittest/test_batch_storage.dat");
}