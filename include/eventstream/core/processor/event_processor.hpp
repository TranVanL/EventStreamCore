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
#include <chrono>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

// ============================================================================
// Base Processor Interface
// ============================================================================

/**
 * @class EventProcessor
 * @brief Abstract interface for event processors.
 *
 * Each concrete processor runs in its own thread, consuming events
 * from a specific EventBus queue and applying domain logic.
 */
class EventProcessor {
public:
    virtual ~EventProcessor() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void process(const EventStream::Event& event) = 0;
    virtual const char* name() const = 0;

    void setNUMANode(int numa_node) { numa_node_ = numa_node; }
    int  getNUMANode() const { return numa_node_; }

protected:
    int numa_node_ = -1;
};

// ============================================================================
// Realtime Processor
// ============================================================================

/**
 * @class RealtimeProcessor
 * @brief Handles CRITICAL / HIGH priority events with strict SLA.
 *
 * Emits alerts for anomalous payloads (e.g. temperature spikes).
 * Events exceeding the latency SLA are sent to the DLQ.
 */
class RealtimeProcessor : public EventProcessor {
public:
    explicit RealtimeProcessor(EventStream::AlertHandlerPtr alert_handler = nullptr,
                               StorageEngine* storage = nullptr,
                               EventStream::DeadLetterQueue* dlq = nullptr);
    ~RealtimeProcessor() noexcept override;

    void start() override;
    void stop() override;
    void process(const EventStream::Event& event) override;
    const char* name() const override { return "RealtimeProcessor"; }

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
    int max_processing_ms_ = 5;
};

// ============================================================================
// Transactional Processor
// ============================================================================

/**
 * @class TransactionalProcessor
 * @brief Processes MEDIUM / LOW priority events with at-least-once guarantee.
 *
 * Features:
 * - Lock-free deduplication for idempotency
 * - Configurable retry with exponential backoff
 * - Latency histogram tracking
 */
class TransactionalProcessor : public EventProcessor {
public:
    explicit TransactionalProcessor(StorageEngine* storage = nullptr,
                                    EventStream::DeadLetterQueue* dlq = nullptr);
    ~TransactionalProcessor() noexcept override;

    void start() override;
    void stop() override;
    void process(const EventStream::Event& event) override;
    const char* name() const override { return "TransactionalProcessor"; }

    void pauseProcessing()  { paused_.store(true,  std::memory_order_release); }
    void resumeProcessing() { paused_.store(false, std::memory_order_release); }

    void setMaxRetries(int retries) { max_retries_ = retries; }
    void setStorage(StorageEngine* storage) { storage_ = storage; }

    EventStream::LatencyHistogram& getLatencyHistogram() { return latency_hist_; }

private:
    std::atomic<bool> paused_{false};
    bool handle(const EventStream::Event& event);

    StorageEngine* storage_ = nullptr;
    EventStream::DeadLetterQueue* dlq_ = nullptr;
    int max_retries_ = 3;

    EventStream::LockFreeDeduplicator dedup_table_;
    std::atomic<uint64_t> last_cleanup_ms_{0};
    EventStream::LatencyHistogram latency_hist_;
};

// ============================================================================
// Batch Processor
// ============================================================================

/**
 * @class BatchProcessor
 * @brief Aggregates BATCH priority events by topic over a time window.
 *
 * Events are buffered per-topic and flushed to storage when the
 * window expires.  Supports controlled dropping via the control plane.
 */
class BatchProcessor : public EventProcessor {
public:
    explicit BatchProcessor(std::chrono::seconds window = std::chrono::seconds(5),
                            EventStream::EventBusMulti* bus = nullptr,
                            StorageEngine* storage = nullptr,
                            EventStream::DeadLetterQueue* dlq = nullptr);
    ~BatchProcessor() noexcept override;

    void start() override;
    void stop() override;
    void process(const EventStream::Event& event) override;
    const char* name() const override { return "BatchProcessor"; }

    void dropBatchEvents()   { drop_events_.store(true,  std::memory_order_release); }
    void resumeBatchEvents() { drop_events_.store(false, std::memory_order_release); }

    void setStorage(StorageEngine* storage) { storage_ = storage; }

private:
    std::atomic<bool> drop_events_{false};
    EventStream::EventBusMulti* event_bus_;
    StorageEngine* storage_ = nullptr;
    EventStream::DeadLetterQueue* dlq_ = nullptr;
    using Clock = std::chrono::steady_clock;

    std::chrono::seconds window_;

    struct TopicBucket {
        alignas(64) std::vector<EventStream::Event> events;
        alignas(64) std::mutex bucket_mutex;
        Clock::time_point last_flush_time{};
    };
    mutable std::mutex buckets_mutex_;
    std::unordered_map<std::string, TopicBucket> buckets_;

    void flush(const std::string& topic);
    void flushBucketLocked(TopicBucket& bucket, const std::string& topic);
};

