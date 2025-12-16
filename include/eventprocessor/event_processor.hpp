#pragma once
#include "event/EventBusMulti.hpp"
#include "eventprocessor/metricRegistry.hpp"
#include <storage_engine/storage_engine.hpp>
#include "utils/spsc_ringBuffer.hpp"
#include "utils/thread_affinity.hpp"
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

    virtual ~RealtimeProcessor();

    virtual void start() override;

    virtual void stop() override;

    virtual void process(const EventStream::Event& event) override;

    virtual const char* name() const override { return "RealtimeProcessor"; }

private: 
    // internal state if needed
    bool handle(const EventStream::Event& event);
    void processingLoop();

    // Lock-free SPSC ring buffer for high-performance real-time processing
    static constexpr size_t RINGBUFFER_CAPACITY = 8192;
    SpscRingBuffer<EventStream::EventPtr, RINGBUFFER_CAPACITY> event_queue_;
    
    std::thread processing_thread_;
    std::atomic<bool> running_{false};
    int cpu_core_{0};  // CPU core for thread affinity

    struct AvoidFalseSharing {
        alignas(64) std::atomic<size_t> value{0};
    };
    AvoidFalseSharing processed_count_;
    AvoidFalseSharing dropped_count_;
    AvoidFalseSharing alert_count_;
};


class TransactionalProcessor : public EventProcessor {
public:
    TransactionalProcessor();

    virtual ~TransactionalProcessor();

    virtual void start() override;

    virtual void stop() override;

    virtual void process(const EventStream::Event& event) override;

    virtual const char* name() const override { return "TransactionalProcessor"; }

private:
    // internal state if needed
    bool handle(const EventStream::Event& event);

    std::unordered_set<uint32_t> processed_event_ids_;
    std::mutex processed_ids_mutex_;
};

class BatchProcessor : public EventProcessor {
public:
    explicit BatchProcessor(std::chrono::seconds window = std::chrono::seconds(5));

    virtual ~BatchProcessor();

    virtual void start() override;

    virtual void stop() override;

    virtual void process(const EventStream::Event& event) override;

    virtual const char* name() const override { return "BatchProcessor"; }

private:
    using Clock = std::chrono::steady_clock;
    
    std::chrono::seconds window_;
    std::map<std::string, std::vector<EventStream::Event>> buckets_;  // topic -> events
    std::map<std::string, Clock::time_point> last_flush_;  // topic -> last flush time
    std::mutex buckets_mutex_;
    
    void flush(const std::string& topic);
};