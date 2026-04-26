#pragma once

#include <eventstream/core/events/event.hpp>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <cstring>
#include <spdlog/spdlog.h>

namespace EventStream {

class ProcessedEventObserver {
public:
    virtual ~ProcessedEventObserver() = default;
    virtual void onEventProcessed(const Event& event, const char* processor_name) = 0;
    virtual void onEventDropped(const Event& event, const char* processor_name,
                                const char* reason) = 0;
    virtual const char* observerName() const { return "unnamed"; }
};

using ProcessedEventObserverPtr = std::shared_ptr<ProcessedEventObserver>;

class ProcessedEventStream {
public:
    static ProcessedEventStream& getInstance() {
        static ProcessedEventStream instance;
        return instance;
    }

    void subscribe(ProcessedEventObserverPtr observer) {
        std::lock_guard<std::mutex> lock(mutex_);
        observers_.push_back(std::move(observer));
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        observers_.clear();
    }

    void notifyProcessed(const Event& event, const char* processor_name) {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        for (auto& obs : snapshot()) {
            try { obs->onEventProcessed(event, processor_name); } catch (...) {}
        }
    }

    void notifyDropped(const Event& event, const char* processor_name, const char* reason) {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        for (auto& obs : snapshot()) {
            try { obs->onEventDropped(event, processor_name, reason); } catch (...) {}
        }
    }

    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }

    bool hasObservers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !observers_.empty();
    }

private:
    ProcessedEventStream() = default;

    std::vector<ProcessedEventObserverPtr> snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return observers_;
    }

    mutable std::mutex mutex_;
    std::vector<ProcessedEventObserverPtr> observers_;
    std::atomic<bool> enabled_{true};
};

// --- Concrete Observers ---

class RealtimeAlertObserver : public ProcessedEventObserver {
public:
    const char* observerName() const override { return "RealtimeAlertObserver"; }

    void onEventProcessed(const Event& event, const char* proc) override {
        if (std::strcmp(proc, "RealtimeProcessor") != 0) return;
        if (event.topic.find("sensor/pressure") == 0)
            spdlog::info("[RT-Observer] pressure event_id={} -> emergency-svc", event.header.id);
        else if (event.topic.find("sensor/temperature") == 0)
            spdlog::info("[RT-Observer] temperature event_id={} -> monitoring", event.header.id);
    }

    void onEventDropped(const Event& event, const char* proc, const char* reason) override {
        if (std::strcmp(proc, "RealtimeProcessor") != 0) return;
        spdlog::warn("[RT-Observer] DROPPED event_id={} reason={}", event.header.id, reason);
    }
};

class TransactionalBusinessObserver : public ProcessedEventObserver {
public:
    const char* observerName() const override { return "TransactionalBusinessObserver"; }

    void onEventProcessed(const Event& event, const char* proc) override {
        if (std::strcmp(proc, "TransactionalProcessor") != 0) return;
        if (event.topic.find("payment") == 0)
            spdlog::info("[Txn-Observer] payment event_id={} -> payment-status", event.header.id);
        else if (event.topic.find("state") == 0)
            spdlog::info("[Txn-Observer] state event_id={} -> state-change", event.header.id);
        else if (event.topic.find("audit") == 0)
            spdlog::info("[Txn-Observer] audit event_id={} -> compliance", event.header.id);
    }

    void onEventDropped(const Event& event, const char* proc, const char* reason) override {
        if (std::strcmp(proc, "TransactionalProcessor") != 0) return;
        spdlog::warn("[Txn-Observer] DROPPED event_id={} reason={}", event.header.id, reason);
    }
};

class BatchAnalyticsObserver : public ProcessedEventObserver {
public:
    const char* observerName() const override { return "BatchAnalyticsObserver"; }

    void onEventProcessed(const Event& event, const char* proc) override {
        if (std::strcmp(proc, "BatchProcessor") != 0) return;
        if (event.topic.find("audit") == 0)
            spdlog::info("[Batch-Observer] audit event_id={} -> analytics", event.header.id);
    }

    void onEventDropped(const Event& event, const char* proc, const char* reason) override {
        if (std::strcmp(proc, "BatchProcessor") != 0) return;
        spdlog::warn("[Batch-Observer] DROPPED event_id={} reason={}", event.header.id, reason);
    }
};

class DroppedEventMonitorObserver : public ProcessedEventObserver {
public:
    const char* observerName() const override { return "DroppedEventMonitorObserver"; }
    void onEventProcessed(const Event&, const char*) override {}

    void onEventDropped(const Event& event, const char* proc, const char* reason) override {
        auto count = drop_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        spdlog::warn("[DropMonitor] proc={} event_id={} reason={} (total={})",
                     proc, event.header.id, reason, count);
    }

    uint64_t getDropCount() const { return drop_count_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> drop_count_{0};
};

inline void registerDefaultObservers() {
    auto& stream = ProcessedEventStream::getInstance();
    stream.subscribe(std::make_shared<RealtimeAlertObserver>());
    stream.subscribe(std::make_shared<TransactionalBusinessObserver>());
    stream.subscribe(std::make_shared<BatchAnalyticsObserver>());
    stream.subscribe(std::make_shared<DroppedEventMonitorObserver>());
    spdlog::info("[ProcessedEventStream] Default observers registered");
}

} // namespace EventStream
