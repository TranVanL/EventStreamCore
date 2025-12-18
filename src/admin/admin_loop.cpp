#include "admin/admin_loop.hpp"

Admin::Admin(ProcessManager& pm) : process_manager_(pm) {}

Admin::~Admin() noexcept {
    spdlog::info("[DESTRUCTOR] Admin being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] Admin destroyed successfully");
}

void Admin::loop() {
    using namespace std::chrono_literals;
    auto& registry = MetricRegistry::getInstance();
    
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(10s);
        
        // CONTROL PLANE: Get metric snapshots (minimal lock)
        auto snapshots = registry.getSnapshots();
        
        spdlog::info("================================ METRICS SNAPSHOT ================================");
        
        // Display health summary
        spdlog::info("[HEALTH STATUS REPORT]");
        int healthy_count = 0, degraded_count = 0, unhealthy_count = 0;
        
        for (const auto& [name, snapshot] : snapshots) {
            HealthStatus status = snapshot.health_status;
            uint64_t drop_rate = snapshot.get_drop_rate_percent();
            bool is_stale = snapshot.is_stale();
            
            const char* status_str = 
                status == HealthStatus::HEALTHY ? "OK" :
                status == HealthStatus::DEGRADED ? "WARN" : "CRIT";
            
            spdlog::info("  [{}] {} (Processed: {}, Drop Rate: {}%, Stale: {})",
                        status_str,
                        name,
                        snapshot.total_events_processed,
                        drop_rate,
                        is_stale ? "YES" : "NO");
            
            if (status == HealthStatus::HEALTHY) healthy_count++;
            else if (status == HealthStatus::DEGRADED) degraded_count++;
            else unhealthy_count++;
        }
        
        spdlog::info("  Summary: {} HEALTHY, {} DEGRADED, {} UNHEALTHY",
                    healthy_count, degraded_count, unhealthy_count);
        
        // Detailed metrics from snapshots
        spdlog::info("[DETAILED METRICS]");
        
        for (const auto& [name, snapshot] : snapshots) {
            if (name == "EventBusMulti") {
                spdlog::info("[{}] Bus Metrics:", name);
                spdlog::info("  Enqueued: {}, Dequeued: {}, Queue Depth: {}",
                            snapshot.total_events_enqueued,
                            snapshot.total_events_dequeued,
                            snapshot.current_queue_depth);
                spdlog::info("  Blocked: {}, Overflow Drops: {}",
                            snapshot.total_events_blocked,
                            snapshot.total_overflow_drops);
            } else {
                spdlog::info("[{}] Processor Metrics:", name);
                spdlog::info("  Processed: {}, Dropped: {} ({:.1f}%)",
                            snapshot.total_events_processed,
                            snapshot.total_events_dropped,
                            static_cast<double>(snapshot.get_drop_rate_percent()));
                
                if (snapshot.event_count_for_avg > 0) {
                    auto avg_latency_us = snapshot.get_avg_latency_ns() / 1000.0;
                    auto max_latency_us = snapshot.max_processing_time_ns / 1000.0;
                    spdlog::info("  Avg Latency: {:.3f} us, Max Latency: {:.3f} us",
                                avg_latency_us, max_latency_us);
                }
            }
        }
        
        spdlog::info("============================================================================");
    }
}