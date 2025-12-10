#include "eventprocessor/realtime_processor.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

RealtimeProcessor::RealtimeProcessor(EventStream::EventBusMulti& bus,
                                     StorageEngine& storage,
                                     ThreadPool* pool)
    : EventProcessor(bus, storage, pool) {}

RealtimeProcessor::~RealtimeProcessor() {
    stop();
}

void RealtimeProcessor::start() {
    isRunning.store(true, std::memory_order_release);
    processingThread = std::thread(&RealtimeProcessor::processLoop, this);
    spdlog::info("RealtimeProcessor started.");
}

void RealtimeProcessor::stop() {
    isRunning.store(false, std::memory_order_release);
    if (processingThread.joinable()) {
        processingThread.join();
    }
    spdlog::info("RealtimeProcessor stopped.");
}

void RealtimeProcessor::processLoop() {
    while (isRunning.load(std::memory_order_acquire)) {
        EventStream::EventPtr evtPtr = nullptr;
        
        // Priority-based consumption: REALTIME > TRANSACTIONAL > BATCH
        // Try REALTIME first (no wait, immediate check)
        auto rtOpt = eventBus.pop(EventStream::EventBusMulti::QueueId::REALTIME,
                                  std::chrono::milliseconds(0));
        if (rtOpt.has_value()) {
            evtPtr = rtOpt.value();
        } else {
            // Try TRANSACTIONAL with short wait
            auto txOpt = eventBus.pop(EventStream::EventBusMulti::QueueId::TRANSACTIONAL,
                                      std::chrono::milliseconds(50));
            if (txOpt.has_value()) {
                evtPtr = txOpt.value();
            } else {
                // Try BATCH with remaining wait
                auto batchOpt = eventBus.pop(EventStream::EventBusMulti::QueueId::BATCH,
                                             std::chrono::milliseconds(50));
                if (batchOpt.has_value()) {
                    evtPtr = batchOpt.value();
                }
            }
        }
        
        if (!evtPtr) continue;

        try {
            if (workerPool) {
                workerPool->submit([evtPtr, this]() {
                    try {
                        storageEngine.storeEvent(*evtPtr);
                        spdlog::debug("RealtimeProcessor stored event ID {} from source type {}",
                                     evtPtr->header.id,
                                     static_cast<int>(evtPtr->header.sourceType));
                    } catch (const std::exception& e) {
                        spdlog::error("RealtimeProcessor failed to store event ID {}: {}",
                                      evtPtr->header.id, e.what());
                    }
                });
            } else {
                storageEngine.storeEvent(*evtPtr);
                spdlog::debug("RealtimeProcessor stored event ID {} from source type {}",
                             evtPtr->header.id,
                             static_cast<int>(evtPtr->header.sourceType));
            }
        } catch (const std::exception& e) {
            spdlog::error("RealtimeProcessor failed to schedule/store event ID {}: {}",
                          evtPtr->header.id, e.what());
        }
    }
}
