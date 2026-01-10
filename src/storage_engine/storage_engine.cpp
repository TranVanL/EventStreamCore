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