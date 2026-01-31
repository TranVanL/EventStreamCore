#pragma once

#include <eventstream/core/events/event.hpp>
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>

namespace EventStream {

/**
 * @class ProcessedEventObserver
 * @brief Observer interface for processed events (optional hook)
 * 
 * This is an OPTIONAL hook for external systems to observe processed events
 * WITHOUT modifying Core logic.
 * 
 * Use cases:
 * - Distributed layer: replicate events to followers
 * - Microservice layer: stream events to gRPC clients
 * - Monitoring: track event flow
 * - Testing: verify processing
 * 
 * Design:
 * - Non-blocking: Implementations MUST NOT block
 * - Thread-safe: May be called from any processor thread
 * - Optional: Core works fine without any observers
 */
class ProcessedEventObserver {
public:
    virtual ~ProcessedEventObserver() = default;
    
    /**
     * @brief Called after event is successfully processed
     * @param event The processed event
     * @param processor_name Which processor handled it
     */
    virtual void onEventProcessed(const Event& event, const char* processor_name) = 0;
    
    /**
     * @brief Called when event is dropped (DLQ, SLA breach, etc.)
     * @param event The dropped event
     * @param processor_name Which processor dropped it
     * @param reason Why it was dropped
     */
    virtual void onEventDropped(const Event& event, const char* processor_name, 
                                 const char* reason) = 0;
};

using ProcessedEventObserverPtr = std::shared_ptr<ProcessedEventObserver>;

/**
 * @class ProcessedEventStream
 * @brief Observable stream of processed events
 * 
 * Singleton that processors call to notify observers.
 * External systems (Distributed, Microservice) subscribe here.
 * 
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                         CORE ENGINE                             │
 * │  Processor ──► ProcessedEventStream.notify()                    │
 * │                         │                                       │
 * └─────────────────────────┼───────────────────────────────────────┘
 *                           │
 *           ┌───────────────┼───────────────┐
 *           ▼               ▼               ▼
 *    DistributedLayer  MicroserviceLayer  Monitoring
 *    (optional)        (optional)         (optional)
 */
class ProcessedEventStream {
public:
    static ProcessedEventStream& getInstance() {
        static ProcessedEventStream instance;
        return instance;
    }
    
    /**
     * @brief Subscribe an observer
     */
    void subscribe(ProcessedEventObserverPtr observer) {
        std::lock_guard<std::mutex> lock(mutex_);
        observers_.push_back(std::move(observer));
    }
    
    /**
     * @brief Unsubscribe all observers (for testing)
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        observers_.clear();
    }
    
    /**
     * @brief Notify all observers of processed event
     * @note Non-blocking, catches exceptions
     */
    void notifyProcessed(const Event& event, const char* processor_name) {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& observer : observers_) {
            try {
                observer->onEventProcessed(event, processor_name);
            } catch (...) {
                // Swallow exceptions - observers must not break Core
            }
        }
    }
    
    /**
     * @brief Notify all observers of dropped event
     */
    void notifyDropped(const Event& event, const char* processor_name, const char* reason) {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& observer : observers_) {
            try {
                observer->onEventDropped(event, processor_name, reason);
            } catch (...) {
                // Swallow exceptions
            }
        }
    }
    
    /**
     * @brief Enable/disable notifications (for performance)
     */
    void setEnabled(bool enabled) {
        enabled_.store(enabled, std::memory_order_relaxed);
    }
    
    bool isEnabled() const {
        return enabled_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Check if any observers are subscribed
     */
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

} // namespace EventStream
