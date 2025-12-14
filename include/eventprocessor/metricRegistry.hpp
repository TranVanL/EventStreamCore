#pragma once
#include "metrics.hpp"
#include <unordered_map>
#include <string>

class MetricRegistry {
public:
    static MetricRegistry& getInstance() {
        static MetricRegistry instance;
        return instance;
    }
    Metrics& getMetrics(const std::string& processorName) {
        return metrics_map_[processorName];
    }
    std::unordered_map<std::string, Metrics> metrics_map_;
private:
    
    MetricRegistry() = default;
    ~MetricRegistry() = default;
};