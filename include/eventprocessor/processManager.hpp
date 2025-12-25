#pragma once

#include "eventprocessor/event_processor.hpp"
#include "metrics/metrics.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <atomic>

class ProcessManager {
public:     
    ProcessManager(EventStream::EventBusMulti& bus);
    ~ProcessManager() noexcept;

    void stop();
    void start(); 

    void runLoop(const EventStream::EventBusMulti::QueueId& qid, EventProcessor* processor);

    // Control plane actions
    void pauseTransactions() const;
    void resumeTransactions() const;
    void dropBatchEvents() const;
    void resumeBatchEvents() const;
    
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