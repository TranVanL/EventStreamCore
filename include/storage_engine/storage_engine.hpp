#pragma once
#include "event/Event.hpp"
#include <string> 
#include <mutex>
#include <fstream>

class StorageEngine {
public:
    StorageEngine(const std::string& storagePath);
    ~StorageEngine();

    void storeEvent(const EventStream::Event& event);
    bool retrieveEvent(uint64_t eventId, EventStream::Event& event);
    void flush();  // Explicit flush for batching

private:
    std::ofstream storageFile;
    std::mutex storageMutex;
    size_t eventCount = 0;
    static constexpr size_t FLUSH_BATCH_SIZE = 100;  // Flush every 100 events
};