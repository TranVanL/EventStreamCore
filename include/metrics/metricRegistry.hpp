#pragma once
#include "metrics.hpp"
#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>

class MetricRegistry {
public:
    struct HealthThresholds {
        uint64_t unhealthy_drop_rate_percent = 10;
        uint64_t unhealthy_error_rate_percent = 5;
        uint64_t unhealthy_queue_depth = 10000;
        uint64_t stale_timeout_ms = 10000;
        uint64_t degraded_drop_rate_percent = 3;
        uint64_t degraded_queue_depth = 5000;
    };
    
    static MetricRegistry& getInstance();
    
    void setHealthThresholds(const HealthThresholds& t);
    const HealthThresholds& getHealthThresholds() const;
    
    Metrics& getMetrics(const std::string& name);
    std::unordered_map<std::string, MetricSnapshot> getSnapshots();
    std::optional<MetricSnapshot> getSnapshot(const std::string& name);
    void updateEventTimestamp(const std::string& name);
    
    /**
     * Lấy aggregate metrics từ tất cả processors + event buses
     * Dùng cho AdminLoop để đưa ra quyết định state
     */
    struct AggregateMetrics {
        uint64_t total_queue_depth;           // Tổng event chưa xử lý
        uint64_t total_dropped;               // Tổng event đã drop
        uint64_t total_processed;             // Tổng event đã xử lý
        double aggregate_drop_rate_percent;   // Drop rate trung bình
        uint64_t max_processor_latency_ns;    // Latency cao nhất
    };
    
    AggregateMetrics getAggregateMetrics() const;
    
    std::unordered_map<std::string, Metrics> metrics_map_;
    
private:
    static uint64_t now();
    MetricSnapshot buildSnapshot(Metrics& m, const HealthThresholds& t, uint64_t ts);
    static HealthStatus checkHealth(uint64_t proc, uint64_t drop, uint64_t err, uint64_t last_ts, uint64_t depth, uint64_t now_ms, const HealthThresholds& t);
    
    mutable std::mutex mtx_metrics_;
    mutable std::mutex mtx_config_;
    HealthThresholds thresholds_;
    
    MetricRegistry() = default;
    ~MetricRegistry() = default;
    MetricRegistry(const MetricRegistry&) = delete;
    MetricRegistry& operator=(const MetricRegistry&) = delete;
    friend class MetricsReporter;
};