#pragma once 
#include <atomic>
#include <cstdint>

struct Metrics {
    // Core metrics only - what we actually use
    std::atomic<uint64_t> total_events_processed{0};    // Total processed
    std::atomic<uint64_t> total_events_dropped{0};      // Total dropped
    std::atomic<uint64_t> current_queue_depth{0};       // Current queue size
    std::atomic<uint64_t> last_event_timestamp_ms{0};   // Last event time
    std::atomic<uint8_t> health_status{0};              // 0=HEALTHY, 1=UNHEALTHY
};

enum class HealthStatus : uint8_t {
    HEALTHY = 0,
    UNHEALTHY = 1
};

struct MetricSnapshot {
    uint64_t total_events_processed;
    uint64_t total_events_dropped;
    uint64_t current_queue_depth;
    HealthStatus health_status;
    
    uint64_t get_drop_rate_percent() const {
        uint64_t total = total_events_processed + total_events_dropped;
        return total > 0 ? (total_events_dropped * 100) / total : 0;
    }
};




