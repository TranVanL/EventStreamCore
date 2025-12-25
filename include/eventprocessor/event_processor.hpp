#pragma once
#include "event/EventBusMulti.hpp"
#include "metrics/metricRegistry.hpp"
#include <storage_engine/storage_engine.hpp>
#include "control/Control_plane.hpp"
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

private:
    ControlPlane control_plane_;
    std::atomic<bool> paused_{false};
    std::atomic<ProcessState> state_{ProcessState::RUNNING};
    bool handle(const EventStream::Event& event);

    struct IdempotencyEntry { uint64_t timestamp_ms; };
    std::unordered_map<uint32_t, IdempotencyEntry> processed_ids_;
    std::mutex processed_ids_mutex_;
    static constexpr uint64_t IDEMPOTENT_WINDOW_MS = 3600000;
    uint64_t last_cleanup_ms_ = 0;
};

class BatchProcessor : public EventProcessor {
public:
    explicit BatchProcessor(std::chrono::seconds window = std::chrono::seconds(5));
    virtual ~BatchProcessor() noexcept;

    virtual void start() override;
    virtual void stop() override;
    virtual void process(const EventStream::Event& event) override;
    virtual const char* name() const override { return "BatchProcessor"; }

    void dropBatchEvents() { drop_events_.store(true, std::memory_order_release); }
    void resumeBatchEvents() { drop_events_.store(false, std::memory_order_release); }

private:
    ControlPlane control_plane_;
    std::atomic<bool> drop_events_{false};
    using Clock = std::chrono::steady_clock;

    std::chrono::seconds window_;
    std::map<std::string, std::vector<EventStream::Event>> buckets_;
    std::map<std::string, Clock::time_point> last_flush_;
    std::mutex buckets_mutex_;

    void flush(const std::string& topic);
};

