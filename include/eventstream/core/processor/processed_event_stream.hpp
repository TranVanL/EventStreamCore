#pragma once

#include <eventstream/core/events/event.hpp>
#include <eventstream/core/processor/event_handler.hpp>
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <spdlog/spdlog.h>

namespace EventStream {

/**
 * @class ProcessedEventObserver
 * @brief Observer interface for processed events.
 *
 * Downstream business logic subscribes here.
 * Observers receive:
 *   - event data
 *   - processor_name (RealtimeProcessor, TransactionalProcessor, BatchProcessor)
 *   - for drops: reason string
 */
class ProcessedEventObserver {
public:
    virtual ~ProcessedEventObserver() = default;
    
    virtual void onEventProcessed(const Event& event, const char* processor_name) = 0;
    virtual void onEventDropped(const Event& event, const char* processor_name, 
                                 const char* reason) = 0;
    virtual const char* observerName() const { return "unnamed"; }
};

using ProcessedEventObserverPtr = std::shared_ptr<ProcessedEventObserver>;

/**
 * @class ProcessedEventStream
 * @brief Observable stream — processors notify here, observers react.
 */
class ProcessedEventStream {
public:
    static ProcessedEventStream& getInstance() {
        static ProcessedEventStream instance;
        return instance;
    }
    
    void subscribe(ProcessedEventObserverPtr observer) {
        std::lock_guard<std::mutex> lock(mutex_);
        spdlog::info("[ProcessedEventStream] Observer subscribed: {}", observer->observerName());
        observers_.push_back(std::move(observer));
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        observers_.clear();
    }
    
    void notifyProcessed(const Event& event, const char* processor_name) {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        
        std::vector<ProcessedEventObserverPtr> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = observers_;
        }
        
        for (auto& observer : snapshot) {
            try {
                observer->onEventProcessed(event, processor_name);
            } catch (...) {}
        }
    }
    
    void notifyDropped(const Event& event, const char* processor_name, const char* reason) {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        
        std::vector<ProcessedEventObserverPtr> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = observers_;
        }
        
        for (auto& observer : snapshot) {
            try {
                observer->onEventDropped(event, processor_name, reason);
            } catch (...) {}
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
    mutable std::mutex mutex_;
    std::vector<ProcessedEventObserverPtr> observers_;
    std::atomic<bool> enabled_{true};
};

// ─── Concrete Observers ─────────────────────────────────────────────

/**
 * @class RealtimeAlertObserver
 * @brief Observes RealtimeProcessor events.
 * sensor/pressure → emergency service | sensor/temperature → monitoring
 */
class RealtimeAlertObserver : public ProcessedEventObserver {
public:
    const char* observerName() const override { return "RealtimeAlertObserver"; }

    void onEventProcessed(const Event& event, const char* processor_name) override {
        if (std::string(processor_name) != "RealtimeProcessor") return;
        
        if (event.topic.find("sensor/pressure") == 0) {
            spdlog::info("[Observer:RealtimeAlert] sensor/pressure event_id={} -> "
                         "dispatch to emergency-service", event.header.id);
        } else if (event.topic.find("sensor/temperature") == 0) {
            spdlog::info("[Observer:RealtimeAlert] sensor/temperature event_id={} -> "
                         "notify monitoring-dashboard", event.header.id);
        } else {
            spdlog::debug("[Observer:RealtimeAlert] realtime event_id={} topic={}", 
                          event.header.id, event.topic);
        }
    }

    void onEventDropped(const Event& event, const char* processor_name, const char* reason) override {
        if (std::string(processor_name) != "RealtimeProcessor") return;
        spdlog::warn("[Observer:RealtimeAlert] DROPPED event_id={} reason={} -> escalate",
                     event.header.id, reason);
    }
};

/**
 * @class TransactionalBusinessObserver
 * @brief payment → payment-status | state → state-change | audit → compliance
 */
class TransactionalBusinessObserver : public ProcessedEventObserver {
public:
    const char* observerName() const override { return "TransactionalBusinessObserver"; }

    void onEventProcessed(const Event& event, const char* processor_name) override {
        if (std::string(processor_name) != "TransactionalProcessor") return;
        
        if (event.topic.find("payment") == 0) {
            spdlog::info("[Observer:TxnBusiness] payment event_id={} -> payment-status svc",
                         event.header.id);
        } else if (event.topic.find("state") == 0) {
            spdlog::info("[Observer:TxnBusiness] state event_id={} -> state-change svc",
                         event.header.id);
        } else if (event.topic.find("audit") == 0) {
            spdlog::info("[Observer:TxnBusiness] audit event_id={} -> compliance svc",
                         event.header.id);
        }
    }

    void onEventDropped(const Event& event, const char* processor_name, const char* reason) override {
        if (std::string(processor_name) != "TransactionalProcessor") return;
        spdlog::warn("[Observer:TxnBusiness] DROPPED event_id={} reason={} -> failed-txn svc",
                     event.header.id, reason);
    }
};

/**
 * @class BatchAnalyticsObserver
 * @brief audit → analytics-pipeline | other → data-warehouse
 */
class BatchAnalyticsObserver : public ProcessedEventObserver {
public:
    const char* observerName() const override { return "BatchAnalyticsObserver"; }

    void onEventProcessed(const Event& event, const char* processor_name) override {
        if (std::string(processor_name) != "BatchProcessor") return;
        
        if (event.topic.find("audit") == 0) {
            spdlog::info("[Observer:BatchAnalytics] audit event_id={} -> analytics-pipeline",
                         event.header.id);
        } else {
            spdlog::debug("[Observer:BatchAnalytics] event_id={} topic={} -> data-warehouse",
                          event.header.id, event.topic);
        }
    }

    void onEventDropped(const Event& event, const char* processor_name, const char* reason) override {
        if (std::string(processor_name) != "BatchProcessor") return;
        spdlog::warn("[Observer:BatchAnalytics] DROPPED event_id={} reason={}",
                     event.header.id, reason);
    }
};

/**
 * @class DroppedEventMonitorObserver
 * @brief Universal drop counter + alerting for all processors
 */
class DroppedEventMonitorObserver : public ProcessedEventObserver {
public:
    const char* observerName() const override { return "DroppedEventMonitorObserver"; }

    void onEventProcessed(const Event&, const char*) override {}

    void onEventDropped(const Event& event, const char* processor_name, const char* reason) override {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[Observer:DropMonitor] proc={} event_id={} topic={} reason={} (total={})", 
                     processor_name, event.header.id, event.topic, reason,
                     drop_count_.load(std::memory_order_relaxed));
    }

    uint64_t getDropCount() const { return drop_count_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> drop_count_{0};
};

/// Register all default observers at startup
inline void registerDefaultObservers() {
    auto& stream = ProcessedEventStream::getInstance();
    stream.subscribe(std::make_shared<RealtimeAlertObserver>());
    stream.subscribe(std::make_shared<TransactionalBusinessObserver>());
    stream.subscribe(std::make_shared<BatchAnalyticsObserver>());
    stream.subscribe(std::make_shared<DroppedEventMonitorObserver>());
    spdlog::info("[ProcessedEventStream] Default observers registered");
}

} // namespace EventStream
