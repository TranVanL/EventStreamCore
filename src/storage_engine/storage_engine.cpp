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
        storageFile.close();
    }
}

void StorageEngine::storeEvent(const EventStream::Event& event) {
    std::lock_guard<std::mutex> lock(storageMutex);
    
    // Simple binary serialization 
    uint64_t ts = static_cast<uint64_t>(event.header.timestamp);
    storageFile.write(reinterpret_cast<const char*>(&ts), sizeof(ts));
    uint8_t source = static_cast<uint8_t>(event.header.sourceType);
    storageFile.write(reinterpret_cast<const char*>(&source), sizeof(source));
    storageFile.write(reinterpret_cast<const char*>(&event.header.id), sizeof(event.header.id));
    // write topic length and topic bytes so topic is persisted
    uint32_t topicLen = static_cast<uint32_t>(event.topic.size());
    storageFile.write(reinterpret_cast<const char*>(&topicLen), sizeof(topicLen));
    if (topicLen > 0) {
        storageFile.write(event.topic.data(), topicLen);
    }

    uint64_t payloadSize = event.body.size();
    storageFile.write(reinterpret_cast<const char*>(&payloadSize), sizeof(payloadSize));
    storageFile.write(reinterpret_cast<const char*>(event.body.data()), payloadSize);
    storageFile.flush();

    if (!storageFile) {
        spdlog::error("Failed to write event {} to storage", event.header.id);
        throw std::runtime_error("Failed to write event to storage");
    }
}