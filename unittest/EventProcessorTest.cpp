#include <gtest/gtest.h>
#include "event/EventFactory.hpp"
#include "event/EventBus.hpp"
#include "eventprocessor/event_processor.hpp"
#include "storage_engine/storage_engine.hpp"
#include "utils/thread_pool.hpp"

TEST(EventProcessor, init) {
    using namespace EventStream;

    EventBus eventBus;
    StorageEngine storageEngine("test_storage.dat");
    ThreadPool workerPool(2);
    EventProcessor eventProcessor(eventBus, storageEngine, &workerPool);

    EXPECT_NO_THROW(eventProcessor.init());
    std::remove("test_storage.dat");
}

TEST(EventProcessor, startStop) {
    using namespace EventStream;

    EventBus eventBus;
    StorageEngine storageEngine("test_storage.dat");
    ThreadPool workerPool(2);
    EventProcessor eventProcessor(eventBus, storageEngine, &workerPool);

    eventProcessor.init();
    EXPECT_NO_THROW(eventProcessor.start());
    EXPECT_NO_THROW(eventProcessor.stop());
    std::remove("test_storage.dat");
}


TEST(EventProcessor, processLoop){
    using namespace EventStream;

    EventBus eventBus;
    StorageEngine storageEngine("test_storage.dat");
    ThreadPool workerPool(2);
    EventProcessor eventProcessor(eventBus, storageEngine, &workerPool);

    eventProcessor.init();
    eventProcessor.start();

    // Create and publish a test event
    std::vector<uint8_t> payload = {0x10, 0x20, 0x30};
    std::unordered_map<std::string, std::string> metadata = {{"key", "value"}};
    Event event = EventFactory::createEvent(EventSourceType::TCP, payload, "test_topic", metadata);

    EXPECT_NO_THROW(eventBus.publishEvent(event));

    // Allow some time for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_NO_THROW(eventProcessor.stop());
    std::remove("test_storage.dat");
}