#pragma once
#include <eventstream/core/metrics/metrics.hpp>
#include <eventstream/core/control/thresholds.hpp>
#include <unordered_map>
#include <string>
#include <string_view>
#include <mutex>
#include <optional>

// Day 39: Compile-time metric name constants to avoid string allocations
namespace MetricNames {
    constexpr std::string_view EVENTBUS = "EventBusMulti";
    constexpr std::string_view REALTIME = "RealtimeProcessor";
    constexpr std::string_view TRANSACTIONAL = "TransactionalProcessor";
    constexpr std::string_view BATCH = "BatchProcessor";
}

class MetricRegistry {
public:
    static MetricRegistry& getInstance();
    
    void setThresholds(const EventStream::ControlThresholds& t);
    const EventStream::ControlThresholds& getThresholds() const;
    
    Metrics& getMetrics(const std::string& name);
    Metrics& getMetrics(std::string_view name);  // Overload for string_view (no allocation)
    Metrics& getMetrics(const char* name);       // Overload for const char* (no allocation)
    std::unordered_map<std::string, MetricSnapshot> getSnapshots();
    std::optional<MetricSnapshot> getSnapshot(const std::string& name);
    void updateEventTimestamp(const std::string& name);
    
private:
    // CRITICAL FIX: metrics_map_ must be private to enforce mutex protection
    std::unordered_map<std::string, Metrics> metrics_map_;
    
    static uint64_t now();
    MetricSnapshot buildSnapshot(Metrics& m, const EventStream::ControlThresholds& t, uint64_t ts);
    
    // CRITICAL FIX: Unified mutex for all registry operations
    // Prevents potential deadlock from separate mutex ordering
    mutable std::mutex mtx_;
    EventStream::ControlThresholds thresholds_;
    
    MetricRegistry() = default;
    ~MetricRegistry() = default;
    MetricRegistry(const MetricRegistry&) = delete;
    MetricRegistry& operator=(const MetricRegistry&) = delete;
    friend class MetricsReporter;
};