#pragma once
#include "eventprocessor/metricRegistry.hpp"
#include <thread>
#include <spdlog/spdlog.h>

class MetricsReporter {
public:
    ~MetricsReporter() noexcept {
        spdlog::info("[DESTRUCTOR] MetricsReporter being destroyed...");
        stop();
        spdlog::info("[DESTRUCTOR] MetricsReporter destroyed successfully");
    }
    
    void start();
    void stop();

private:
    void loop();

private:
    std::thread worker_;
    std::atomic<bool> running_{false};
};
