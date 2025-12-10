#include <gtest/gtest.h>
#include "event/EventFactory.hpp"
#include "event/EventBusMulti.hpp"
#include "eventprocessor/realtime_processor.hpp"
#include "storage_engine/storage_engine.hpp"
#include "utils/thread_pool.hpp"

TEST(EventProcessor, init) {
    using namespace EventStream;

    EventBusMulti eventBus;
    StorageEngine storageEngine("unittest/test_storage.dat");
    ThreadPool workerPool(2);
    RealtimeProcessor eventProcessor(eventBus, storageEngine, &workerPool);

    // RealtimeProcessor initializes in constructor
    std::remove("unittest/test_storage.dat");
}

TEST(EventProcessor, startStop) {
    using namespace EventStream;

    EventBusMulti eventBus;
    StorageEngine storageEngine("unittest/test_storage.dat");
    ThreadPool workerPool(2);
    RealtimeProcessor eventProcessor(eventBus, storageEngine, &workerPool);

    eventProcessor.start();
    eventProcessor.stop();
    std::remove("unittest/test_storage.dat");
}


TEST(EventProcessor, processLoop){
    using namespace EventStream;

    EventBusMulti eventBus;
    StorageEngine storageEngine("unittest/test_storage.dat");
    ThreadPool workerPool(2);
    RealtimeProcessor eventProcessor(eventBus, storageEngine, &workerPool);

    eventProcessor.start();

    // Create and publish a test event
    std::vector<uint8_t> payload = {0x10, 0x20, 0x30};
    std::unordered_map<std::string, std::string> metadata = {{"key", "value"}};
    Event event = EventFactory::createEvent(EventSourceType::TCP,EventPriority::MEDIUM, std::move(payload), "test_topic", std::move(metadata));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    eventProcessor.stop();
    std::remove("unittest/test_storage.dat");
}