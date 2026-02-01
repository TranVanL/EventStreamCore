#pragma once
#include <eventstream/core/events/event_bus.hpp>
#include <eventstream/core/events/dead_letter_queue.hpp>
#include <eventstream/core/metrics/registry.hpp>
#include <eventstream/core/metrics/histogram.hpp>
#include <eventstream/core/memory/numa_binding.hpp>
#include <eventstream/core/storage/storage_engine.hpp>
#include <eventstream/core/control/control_plane.hpp>
#include <eventstream/core/queues/lock_free_dedup.hpp>
#include <eventstream/core/processor/alert_handler.hpp>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>
#include <set>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <map>
#include <vector>
#include <memory>
#include <functional>

// Processor execution state (Day 23)
enum class ProcessorState {
    RUNNING = 0,    // Normal operation
    PAUSED = 1,     // Stop consuming, queue grows
    DRAINING = 2    // Finish current work, then pause
};

enum class ProcessState {
    RUNNING = 0,
    STOPPED = 1,
    PAUSED = 2
};

class EventProcessor {
public:

    virtual ~EventProcessor() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void process(const EventStream::Event& event) = 0;

    // processor type (debug / metrics)
    virtual const char* name() const = 0;

    /**
     * @brief Set NUMA node binding for processor thread (Day 38)
     * @param numa_node NUMA node ID (-1 to disable)
     */
    virtual void setNUMANode(int numa_node) { numa_node_ = numa_node; }

    /**
     * @brief Get current NUMA node binding
     */
    virtual int getNUMANode() const { return numa_node_; }

protected:
    int numa_node_ = -1;  // NUMA node binding (-1 = no binding)
};



class RealtimeProcessor : public EventProcessor {
public:
    /**
     * @brief Construct RealtimeProcessor with optional dependencies
     * @param alert_handler Handler for alerts (nullptr = use default logging)
     * @param storage Storage engine for audit trail (nullptr = no persistence)
     * @param dlq Dead letter queue for dropped events (nullptr = no DLQ)
     */
    explicit RealtimeProcessor(EventStream::AlertHandlerPtr alert_handler = nullptr,
                               StorageEngine* storage = nullptr,
                               EventStream::DeadLetterQueue* dlq = nullptr);
    virtual ~RealtimeProcessor() noexcept;

    virtual void start() override;
    virtual void stop() override;
    virtual void process(const EventStream::Event& event) override;
    virtual const char* name() const override {
        return "RealtimeProcessor";
    }
    
    // Configuration
    void setMaxProcessingMs(int ms) { max_processing_ms_ = ms; }
    void setAlertHandler(EventStream::AlertHandlerPtr handler) { alert_handler_ = std::move(handler); }
    void setStorage(StorageEngine* storage) { storage_ = storage; }

private:
    bool handle(const EventStream::Event& event);
    void emitAlert(EventStream::AlertLevel level, const std::string& message,
                   const EventStream::Event& event);
    
    EventStream::AlertHandlerPtr alert_handler_;
    StorageEngine* storage_ = nullptr;
    EventStream::DeadLetterQueue* dlq_ = nullptr;
    int max_processing_ms_ = 5;  // Configurable SLA

    struct AvoidFalseSharing {
        alignas(64) std::atomic<size_t> value{0};
    };
};


class TransactionalProcessor : public EventProcessor {
public:
    /**
     * @brief Construct TransactionalProcessor with dependencies
     * @param storage Storage engine for durable writes (nullptr = no persistence)
     * @param dlq Dead letter queue for failed events (nullptr = no DLQ)
     */
    explicit TransactionalProcessor(StorageEngine* storage = nullptr,
                                    EventStream::DeadLetterQueue* dlq = nullptr);
    virtual ~TransactionalProcessor() noexcept;

    virtual void start() override;
    virtual void stop() override;
    virtual void process(const EventStream::Event& event) override;
    virtual const char* name() const override { return "TransactionalProcessor"; }

    void pauseProcessing() { paused_.store(true, std::memory_order_release); }
    void resumeProcessing() { paused_.store(false, std::memory_order_release); }
    
    // Configuration
    void setMaxRetries(int retries) { max_retries_ = retries; }
    void setStorage(StorageEngine* storage) { storage_ = storage; }

    /**
     * @brief Get reference to latency histogram (Day 37)
     * Tracks dequeue -> processed latency
     */
    EventStream::LatencyHistogram& getLatencyHistogram() { return latency_hist_; }

private:
    std::atomic<bool> paused_{false};
    std::atomic<ProcessState> state_{ProcessState::RUNNING};
    bool handle(const EventStream::Event& event);
    
    StorageEngine* storage_ = nullptr;
    EventStream::DeadLetterQueue* dlq_ = nullptr;
    int max_retries_ = 3;

    // Optimized lock-free deduplication (Day 34)
    EventStream::LockFreeDeduplicator dedup_table_;
    std::atomic<uint64_t> last_cleanup_ms_{0};

    // Day 37: Latency histogram for tail latency measurement
    EventStream::LatencyHistogram latency_hist_;
};

class BatchProcessor : public EventProcessor {
public:
    /**
     * @brief Construct BatchProcessor with dependencies
     * @param window Batch window duration
     * @param bus Event bus for queue operations
     * @param storage Storage engine for batch writes (nullptr = no persistence)
     * @param dlq Dead letter queue for dropped events (nullptr = no DLQ)
     */
    explicit BatchProcessor(std::chrono::seconds window = std::chrono::seconds(5),
                          EventStream::EventBusMulti* bus = nullptr,
                          StorageEngine* storage = nullptr,
                          EventStream::DeadLetterQueue* dlq = nullptr);
    virtual ~BatchProcessor() noexcept;

    virtual void start() override;
    virtual void stop() override;
    virtual void process(const EventStream::Event& event) override;
    virtual const char* name() const override { return "BatchProcessor"; }

    void dropBatchEvents() { drop_events_.store(true, std::memory_order_release); }
    void resumeBatchEvents() { drop_events_.store(false, std::memory_order_release); }
    
    // Configuration
    void setStorage(StorageEngine* storage) { storage_ = storage; }

private:
    std::atomic<bool> drop_events_{false};
    EventStream::EventBusMulti* event_bus_;
    StorageEngine* storage_ = nullptr;
    EventStream::DeadLetterQueue* dlq_ = nullptr;
    using Clock = std::chrono::steady_clock;

    std::chrono::seconds window_;
    
    // Day 39 Optimization: Combine last_flush timestamp into TopicBucket
    // Reduces separate map lookup from O(2) to O(1) per event
    struct TopicBucket {
        alignas(64) std::vector<EventStream::Event> events;
        alignas(64) std::mutex bucket_mutex;  // Fine-grained lock per bucket
        Clock::time_point last_flush_time{};  // COMBINED: Track flush time per bucket
    };
    mutable std::mutex buckets_mutex_;  // CRITICAL FIX: Protect map itself from reallocation
    std::unordered_map<std::string, TopicBucket> buckets_;

    void flush(const std::string& topic);
    void flushBucketLocked(TopicBucket& bucket, const std::string& topic);
};

