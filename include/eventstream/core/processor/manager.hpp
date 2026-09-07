#pragma once

#include <eventstream/core/processor/processor.hpp>
#include <eventstream/core/metrics/metrics.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <atomic>
#include <memory>
#include <thread>
#include <eventstream/rt/rt_policy.hpp>

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

    eventstream::rt::RtPolicy realtimePolicy_ =
        eventstream::rt::RtPolicyBuilder().fifo().priority(80).cpus({2}).build();
    eventstream::rt::RtPolicy transactionalPolicy_ =
        eventstream::rt::RtPolicyBuilder().fifo().priority(50).cpus({3}).build();
    eventstream::rt::RtPolicy batchPolicy_ =
        eventstream::rt::RtPolicyBuilder().fifo().priority(40).cpus({3}).build();
};