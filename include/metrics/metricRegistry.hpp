#pragma once
#include "metrics.hpp"
#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>

/**
 * CONTROL PLANE: MetricRegistry manages all metrics
 * - DATA PLANE (Metrics): lock-free atomics for event streams
 * - CONTROL PLANE (Snapshots): minimal-lock snapshot capture for reporting/health decisions
 */
class MetricRegistry {
public:
    static MetricRegistry& getInstance() {
        static MetricRegistry instance;
        return instance;
    }
    
    // DATA PLANE: Get metrics reference for direct atomic updates (fast path)
    Metrics& getMetrics(const std::string& processorName) {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        return metrics_map_[processorName];
    }
    
    // CONTROL PLANE: Get snapshot of all metrics (lock held briefly)
    std::unordered_map<std::string, MetricSnapshot> getSnapshots() {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        std::unordered_map<std::string, MetricSnapshot> snapshots;
        
        for (const auto& [name, metrics] : metrics_map_) {
            snapshots[name] = createSnapshot(metrics);
        }
        return snapshots;
    }
    
    // CONTROL PLANE: Get single snapshot for a component
    std::optional<MetricSnapshot> getSnapshot(const std::string& componentName) {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        auto it = metrics_map_.find(componentName);
        if (it == metrics_map_.end()) return std::nullopt;
        return createSnapshot(it->second);
    }
    
    // CONTROL PLANE: Health check result for AdminLoop decision making
    HealthCheckResult getHealthStatus(const std::string& componentName) {
        auto snapshot = getSnapshot(componentName);
        
        HealthCheckResult result;
        result.component_name = componentName;
        
        if (!snapshot) {
            result.status = HealthStatus::UNHEALTHY;
            result.processed_count = 0;
            result.error_count = 0;
            result.drop_rate_percent = 0;
            result.is_stale = true;
            result.diagnosis = "Component not found";
            return result;
        }
        
        result.status = snapshot->health_status;
        result.processed_count = snapshot->total_events_processed;
        result.error_count = snapshot->total_events_errors;
        result.drop_rate_percent = snapshot->get_drop_rate_percent();
        result.is_stale = snapshot->is_stale();
        
        // Generate diagnostic
        if (result.is_stale) {
            result.diagnosis = "STALE: No events for >10s";
        } else if (result.status == HealthStatus::UNHEALTHY) {
            result.diagnosis = "CRITICAL: Health status = UNHEALTHY";
        } else if (result.drop_rate_percent > 10) {
            result.diagnosis = "WARN: Drop rate > 10%";
        } else if (result.error_count > 0) {
            result.diagnosis = "INFO: Errors detected";
        } else {
            result.diagnosis = "OK: Operating normally";
        }
        
        return result;
    }
    
    // DATA PLANE: Update last event timestamp (called by processors after processing event)
    void updateEventTimestamp(const std::string& processorName) {
        auto now = std::chrono::system_clock::now();
        uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        auto it = metrics_map_.find(processorName);
        if (it != metrics_map_.end()) {
            it->second.last_event_timestamp_ms.store(now_ms, std::memory_order_relaxed);
        }
    }
    
    // For metrics reporter to iterate - already thread-safe with lock above
    std::unordered_map<std::string, Metrics> metrics_map_;
    
private:
    // Create snapshot from metrics (must be called under lock)
    // No atomics in the snapshot - just copied values at this moment
    static MetricSnapshot createSnapshot(const Metrics& metrics) {
        MetricSnapshot snap;
        snap.total_events_processed = metrics.total_events_processed.load(std::memory_order_relaxed);
        snap.total_events_dropped = metrics.total_events_dropped.load(std::memory_order_relaxed);
        snap.total_events_alerted = metrics.total_events_alerted.load(std::memory_order_relaxed);
        snap.total_events_errors = metrics.total_events_errors.load(std::memory_order_relaxed);
        snap.total_events_skipped = metrics.total_events_skipped.load(std::memory_order_relaxed);
        snap.total_retries = metrics.total_retries.load(std::memory_order_relaxed);
        snap.total_events_enqueued = metrics.total_events_enqueued.load(std::memory_order_relaxed);
        snap.total_events_dequeued = metrics.total_events_dequeued.load(std::memory_order_relaxed);
        snap.total_events_blocked = metrics.total_events_blocked.load(std::memory_order_relaxed);
        snap.total_overflow_drops = metrics.total_overflow_drops.load(std::memory_order_relaxed);
        snap.total_processing_time_ns = metrics.total_processing_time_ns.load(std::memory_order_relaxed);
        snap.max_processing_time_ns = metrics.max_processing_time_ns.load(std::memory_order_relaxed);
        snap.event_count_for_avg = metrics.event_count_for_avg.load(std::memory_order_relaxed);
        snap.last_event_timestamp_ms = metrics.last_event_timestamp_ms.load(std::memory_order_relaxed);
        snap.current_queue_depth = metrics.current_queue_depth.load(std::memory_order_relaxed);
        snap.health_status = static_cast<HealthStatus>(metrics.health_status.load(std::memory_order_relaxed));
        return snap;
    }
    
    mutable std::mutex metrics_mutex_;
    
    MetricRegistry() = default;
    ~MetricRegistry() = default;
    
    // Prevent copying
    MetricRegistry(const MetricRegistry&) = delete;
    MetricRegistry& operator=(const MetricRegistry&) = delete;
    
    friend class MetricsReporter;
};