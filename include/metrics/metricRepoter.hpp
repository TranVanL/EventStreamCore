#pragma once
#include "metrics/metricRegistry.hpp"
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>

/**
 * CONTROL PLANE: Metrics Reporter
 * Periodically captures and reports metrics snapshots
 */
class MetricsReporter {
public:
    MetricsReporter() = default;
    ~MetricsReporter() noexcept = default;
    
    void start();
    void stop();
    
private:
    void loop();
    
    std::atomic<bool> running_{false};
    std::thread worker_;
};