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
        std::this_thread::sleep_for(5s);
        
        spdlog::info("========================================================");
        spdlog::info("         METRICS SNAPSHOT (5-second interval)          ");
        spdlog::info("========================================================");
        
        auto& registry = MetricRegistry::getInstance();
        
        for (const auto& [name, metrics] : registry.metrics_map_) {
            if (name == "EventBusMulti") {
                spdlog::info("");
                spdlog::info("[{}]", name);
                spdlog::info("  +- [Bus] Enqueued:  {} events", metrics.total_events_enqueued.load());
                spdlog::info("  +- [Bus] Dequeued:  {} events", metrics.total_events_dequeued.load());
                spdlog::info("  +- [Bus] Blocked:   {} times (producer blocking)", metrics.total_events_blocked.load());
                spdlog::info("  +- [Bus] Overflows: {} events dropped due to overflow", metrics.total_overflow_drops.load());
            }
            else {
                spdlog::info("");
                spdlog::info("[{}]", name);
                spdlog::info("  +- Processed:  {} events", metrics.total_events_processed.load());
                spdlog::info("  +- Dropped:    {} events", metrics.total_events_dropped.load());
                spdlog::info("  +- Alerted:    {} events", metrics.total_events_alerted.load());
                spdlog::info("  +- Errors:     {} events", metrics.total_events_errors.load());
                spdlog::info("  +- Skipped:    {} events (duplicates/idempotent)", metrics.total_events_skipped.load());            
            // Show retry metrics if transactional
            if (metrics.total_retries.load() > 0) {
                spdlog::info("  +- Retries:    {} attempts", metrics.total_retries.load());
            }            }
            // Calculate average processing time if available
            if (metrics.event_count_for_avg.load() > 0) {
                uint64_t avg_time_ns = metrics.total_processing_time_ns.load() / metrics.event_count_for_avg.load();
                uint64_t max_time_ns = metrics.max_processing_time_ns.load();
                
                spdlog::info("  |");
                spdlog::info("  +- Avg latency:     {} ns ({} us)", avg_time_ns, avg_time_ns / 1000);
                spdlog::info("  +- Max latency:     {} ns ({} us)", max_time_ns, max_time_ns / 1000);
            }
        }
        
        spdlog::info("========================================================");
    }
}