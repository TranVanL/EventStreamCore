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
    static HealthStatus checkHealth(uint64_t proc, uint64_t drop, uint64_t depth, uint64_t last_ts, uint64_t stale_timeout_ms, uint64_t now_ms);
    
    mutable std::mutex mtx_metrics_;
    mutable std::mutex mtx_config_;
    EventStream::ControlThresholds thresholds_;
    
    MetricRegistry() = default;
    ~MetricRegistry() = default;
    MetricRegistry(const MetricRegistry&) = delete;
    MetricRegistry& operator=(const MetricRegistry&) = delete;
    friend class MetricsReporter;
};