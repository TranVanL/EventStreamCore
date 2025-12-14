#pragma once
#include "eventprocessor/metricRegistry.hpp"
#include <thread>

class MetricsReporter {
public:
    void start();
    void stop();

private:
    void loop();

private:
    std::thread worker_;
    std::atomic<bool> running_{false};
};
