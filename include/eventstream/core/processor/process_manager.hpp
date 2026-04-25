#pragma once

#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/metrics/metrics.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <atomic>

class ProcessManager {
public:
    struct Dependencies {
        StorageEngine* storage = nullptr;
        EventStream::DeadLetterQueue* dlq = nullptr;
        EventStream::AlertHandlerPtr alert_handler = nullptr;
        std::chrono::seconds batch_window{5};
    };

    explicit ProcessManager(EventStream::EventBusMulti& bus);
    ProcessManager(EventStream::EventBusMulti& bus, const Dependencies& deps);
    ~ProcessManager() noexcept;

    void start();
    void stop();
    void runLoop(const EventStream::EventBusMulti::QueueId& qid, EventProcessor* processor);

    void pauseTransactions() const;
    void resumeTransactions() const;
    void dropBatchEvents() const;
    void resumeBatchEvents() const;

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
