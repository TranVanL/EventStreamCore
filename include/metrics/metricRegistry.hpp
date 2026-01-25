#pragma once
#include "metrics.hpp"
#include "control/ControlThresholds.hpp"
#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>

class MetricRegistry {
public:
    static MetricRegistry& getInstance();
    
    void setThresholds(const EventStream::ControlThresholds& t);
    const EventStream::ControlThresholds& getThresholds() const;
    
    Metrics& getMetrics(const std::string& name);
    std::unordered_map<std::string, MetricSnapshot> getSnapshots();
    std::optional<MetricSnapshot> getSnapshot(const std::string& name);
    void updateEventTimestamp(const std::string& name);
    
    std::unordered_map<std::string, Metrics> metrics_map_;
    
private:
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