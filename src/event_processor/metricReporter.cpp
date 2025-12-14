#include "eventprocessor/metricRepoter.hpp"
#include <spdlog/spdlog.h>
void MetricsReporter::start() {
    running_.store(true,std::memory_order_release);
    worker_ = std::thread(&MetricsReporter::loop,this);
}

void MetricsReporter::stop() {
    running_.store(false,std::memory_order_release);
    if(worker_.joinable()) {
        worker_.join();
    }
}   

void MetricsReporter::loop() {
    using namespace std::chrono_literals;
    while(running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(20s);
        
        spdlog::info("===== METRICS SNAPSHOT =====");
        auto& registry = MetricRegistry::getInstance();
        for (const auto& [name, metrics] : registry.metrics_map_) {
            spdlog::info("[{}] : Processed {} ,Dropped {} ,Alerted {} ,Skipped {}", name, metrics.total_events_processed.load(), metrics.total_events_dropped.load(), metrics.total_events_alerted.load(), metrics.total_events_skipped.load());
        }
        spdlog::info("==========================");
    }
}