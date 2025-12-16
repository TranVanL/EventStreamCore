#pragma once
#include "metrics.hpp"
#include <unordered_map>
#include <string>
#include <mutex>

class MetricRegistry {
public:
    static MetricRegistry& getInstance() {
        static MetricRegistry instance;
        return instance;
    }
    
    Metrics& getMetrics(const std::string& processorName) {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        return metrics_map_[processorName];
    }
    
    // For metrics reporter to iterate - already thread-safe with lock above
    std::unordered_map<std::string, Metrics> metrics_map_;
    
private:
    mutable std::mutex metrics_mutex_;
    
    MetricRegistry() = default;
    ~MetricRegistry() = default;
    
    // Prevent copying
    MetricRegistry(const MetricRegistry&) = delete;
    MetricRegistry& operator=(const MetricRegistry&) = delete;
    
    friend class MetricsReporter;
};