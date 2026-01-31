#pragma once

#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/metrics/metrics.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <atomic>

/**
 * @class ProcessManager
 * @brief Manages processor lifecycle and wires dependencies
 * 
 * Dependencies:
 * - EventBusMulti: Source of events for all processors
 * - StorageEngine: Durable persistence for Trans/Batch (optional)
 * - DeadLetterQueue: Dropped events tracking (optional)
 * - AlertHandler: Alert callbacks for Realtime (optional)
 */
class ProcessManager {
public:
    /**
     * @brief Dependencies for processors (all optional except event_bus)
     */
    struct Dependencies {
        StorageEngine* storage = nullptr;
        EventStream::DeadLetterQueue* dlq = nullptr;
        EventStream::AlertHandlerPtr alert_handler = nullptr;
        std::chrono::seconds batch_window{5};
    };
    
    /**
     * @brief Construct with event bus only (backward compatible)
     */
    explicit ProcessManager(EventStream::EventBusMulti& bus);
    
    /**
     * @brief Construct with all dependencies
     */
    ProcessManager(EventStream::EventBusMulti& bus, const Dependencies& deps);
    
    ~ProcessManager() noexcept;

    void stop();
    void start(); 

    void runLoop(const EventStream::EventBusMulti::QueueId& qid, EventProcessor* processor);

    // Control plane actions
    void pauseTransactions() const;
    void resumeTransactions() const;
    void dropBatchEvents() const;
    void resumeBatchEvents() const;
    
    // Getter for event bus (used by control plane)
    EventStream::EventBusMulti& getEventBus() { return event_bus; }
    
    void printLatencyMetrics() const;
    
private: 
    EventStream::EventBusMulti& event_bus;
    std::atomic<bool> isRunning_;

    std::unique_ptr<RealtimeProcessor> realtimeProcessor_;
    std::unique_ptr<TransactionalProcessor> transactionalProcessor_;
    std::unique_ptr<BatchProcessor> batchProcessor_;

    std::thread realtimeThread_;
    std::thread transactionalThread_;
    std::thread batchThread_;
};