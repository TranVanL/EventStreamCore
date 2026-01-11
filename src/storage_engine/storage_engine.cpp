#include "storage_engine/storage_engine.hpp"
#include <spdlog/spdlog.h>
#include <cstdint>
#include <stdexcept>

StorageEngine::StorageEngine(const std::string& storagePath) {
    storageFile.open(storagePath, std::ios::binary | std::ios::app);
    if (!storageFile.is_open()) {
        spdlog::error("Failed to open storage file at {}", storagePath);
        throw std::runtime_error("Failed to open storage file");
    }
}

StorageEngine::~StorageEngine() {
    if (storageFile.is_open()) {
        storageFile.flush();
        storageFile.close();
    }
}

void StorageEngine::flush() {
    std::lock_guard<std::mutex> lock(storageMutex);
    if (storageFile.is_open()) {
        storageFile.flush();
    }
}

void StorageEngine::storeEvent(const EventStream::Event& event) {
    std::lock_guard<std::mutex> lock(storageMutex);
    
    // Batch serialize to buffer (zero-copy optimization - single contiguous write)
    std::vector<uint8_t> buffer;
    
    // Estimate capacity to avoid reallocations
    size_t estimatedSize = sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint32_t) 
                         + sizeof(uint32_t) + sizeof(uint64_t) 
                         + event.topic.size() + event.body.size();
    buffer.reserve(estimatedSize);
    
    // Write header fields to buffer (no individual write calls)
    uint64_t ts = static_cast<uint64_t>(event.header.timestamp);
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&ts), 
                  reinterpret_cast<const uint8_t*>(&ts) + sizeof(ts));
    
    uint8_t source = static_cast<uint8_t>(event.header.sourceType);
    buffer.push_back(source);
    
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&event.header.id), 
                  reinterpret_cast<const uint8_t*>(&event.header.id) + sizeof(event.header.id));
    
    uint32_t topicLen = static_cast<uint32_t>(event.topic.size());
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&topicLen), 
                  reinterpret_cast<const uint8_t*>(&topicLen) + sizeof(topicLen));
    
    if (topicLen > 0) {
        buffer.insert(buffer.end(), event.topic.begin(), event.topic.end());
    }
    
    uint64_t payloadSize = event.body.size();
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&payloadSize), 
                  reinterpret_cast<const uint8_t*>(&payloadSize) + sizeof(payloadSize));
    
    buffer.insert(buffer.end(), event.body.begin(), event.body.end());
    
    // Single write call instead of 6+ calls
    storageFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    
    if (!storageFile.good()) {
        spdlog::error("Failed to write event {} to storage", event.header.id);
        throw std::runtime_error("Failed to write event to storage");
    }
    
    // Batch flush: only flush every N events instead of every event
    ++eventCount;
    if (eventCount >= FLUSH_BATCH_SIZE) {
        storageFile.flush();
        eventCount = 0;
    }
}

bool StorageEngine::retrieveEvent(uint64_t eventId, EventStream::Event& event) {
    // Cannot use the same file stream for both read and write
    // This is a simplified implementation - in production, use a proper database
    spdlog::warn("retrieveEvent: Storage layer is append-only for write operations. Use database for random access reads.");
    return false;
}

// ============================================================================
// Day 23: DLQ Storage Implementation
// ============================================================================

void StorageEngine::appendDLQ(const std::vector<EventStream::EventPtr>& events, const std::string& reason) {
    if (events.empty()) return;
    
    std::lock_guard<std::mutex> lock(storageMutex);
    
    // Open DLQ file if not already open
    if (!dlqFile.is_open()) {
        std::string dlqPath = "dlq_log.txt";
        dlqFile.open(dlqPath, std::ios::app);
        if (!dlqFile.is_open()) {
            spdlog::error("[StorageEngine] Failed to open DLQ file");
            return;
        }
    }
    
    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);
    uint64_t timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    
    // Write each dropped event to DLQ log
    for (const auto& evt : events) {
        if (!evt) continue;
        
        // Format: [timestamp] DROPPED: id=X topic=Y priority=Z reason=W
        char buffer[512];
        snprintf(buffer, sizeof(buffer),
                 "[%ld] DROPPED: id=%u topic=%s priority=%d reason=%s\n",
                 timestamp,
                 evt->header.id,
                 evt->topic.c_str(),
                 static_cast<int>(evt->header.priority),
                 reason.c_str());
        
        dlqFile << buffer;
    }
    
    dlqFile.flush();
    
    // Update DLQ statistics
    dlq_count_ += events.size();
    last_dlq_reason_ = reason;
    last_dlq_timestamp_ms_ = timestamp_ms;
    
    spdlog::warn("[StorageEngine] Appended {} events to DLQ: {}", 
                 events.size(), reason);
}

StorageEngine::DLQStats StorageEngine::getDLQStats() const {
    // Cannot use lock_guard with const, so just return values
    // In production, use mutable member for mutex
    return {
        dlq_count_,
        last_dlq_reason_,
        last_dlq_timestamp_ms_
    };
}
