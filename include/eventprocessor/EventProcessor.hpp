#pragma once
#include "event/EventBusMulti.hpp"
#include "metrics/MetricRegistry.hpp"
#include "metrics/latency_histogram.hpp"
#include "core/numa_binding.hpp"
#include <storage_engine/storage_engine.hpp>
#include "control/ControlPlane.hpp"
#include "utils/lock_free_dedup.hpp"
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
    RealtimeProcessor();
    virtual ~RealtimeProcessor() noexcept;

    virtual void start() override;
    virtual void stop() override;
    virtual void process(const EventStream::Event& event) override;
    virtual const char* name() const override {
        return "RealtimeProcessor";
    }

private:
    bool handle(const EventStream::Event& event);

    struct AvoidFalseSharing {
        alignas(64) std::atomic<size_t> value{0};
    };
};


class TransactionalProcessor : public EventProcessor {
public:
    TransactionalProcessor();
    virtual ~TransactionalProcessor() noexcept;

    virtual void start() override;
    virtual void stop() override;
    virtual void process(const EventStream::Event& event) override;
    virtual const char* name() const override { return "TransactionalProcessor"; }

    void pauseProcessing() { paused_.store(true, std::memory_order_release); }
    void resumeProcessing() { paused_.store(false, std::memory_order_release); }

    /**
     * @brief Get reference to latency histogram (Day 37)
     * Tracks dequeue -> processed latency
     */
    EventStream::LatencyHistogram& getLatencyHistogram() { return latency_hist_; }

private:
    std::atomic<bool> paused_{false};
    std::atomic<ProcessState> state_{ProcessState::RUNNING};
    bool handle(const EventStream::Event& event);

    // Optimized lock-free deduplication (Day 34)
    EventStream::LockFreeDeduplicator dedup_table_;
    std::atomic<uint64_t> last_cleanup_ms_{0};

    // Day 37: Latency histogram for tail latency measurement
    EventStream::LatencyHistogram latency_hist_;
};

class BatchProcessor : public EventProcessor {
public:
    explicit BatchProcessor(std::chrono::seconds window = std::chrono::seconds(5),
                          EventStream::EventBusMulti* bus = nullptr);
    virtual ~BatchProcessor() noexcept;

    virtual void start() override;
    virtual void stop() override;
    virtual void process(const EventStream::Event& event) override;
    virtual const char* name() const override { return "BatchProcessor"; }

    void dropBatchEvents() { drop_events_.store(true, std::memory_order_release); }
    void resumeBatchEvents() { drop_events_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> drop_events_{false};
    EventStream::EventBusMulti* event_bus_;
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
};

