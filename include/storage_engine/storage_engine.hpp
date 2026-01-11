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
    
    // Day 23: DLQ storage (persistent dropped events)
    void appendDLQ(const std::vector<EventStream::EventPtr>& events, const std::string& reason);
    
    struct DLQStats {
        size_t total_dropped;
        std::string last_drop_reason;
        uint64_t last_drop_timestamp_ms;
    };
    
    DLQStats getDLQStats() const;

private:
    std::ofstream storageFile;
    std::ofstream dlqFile;  // Day 23: DLQ log file
    std::mutex storageMutex;
    size_t eventCount = 0;
    size_t dlq_count_ = 0;  // Day 23: DLQ event counter
    std::string last_dlq_reason_;  // Day 23: Last drop reason
    uint64_t last_dlq_timestamp_ms_ = 0;  // Day 23: Last drop time
    static constexpr size_t FLUSH_BATCH_SIZE = 100;  // Flush every 100 events
};