#pragma once 
#include <atomic>
#include <cstdint>
#include <chrono>

/**
 * @brief Metrics tracking structure for processors and event bus
 * 
 * All counters are lock-free atomic for minimal overhead.
 * Memory ordering: relaxed (no synchronization overhead, acceptable for metrics)
 * 
 * DATA PLANE: Thread-safe lock-free updates from all threads
 */
struct Metrics {
    // Processor-level metrics
    std::atomic<uint64_t> total_events_processed{0};     // Events successfully processed
    std::atomic<uint64_t> total_events_dropped{0};       // Events dropped due to SLA/errors/overflow
    std::atomic<uint64_t> total_events_alerted{0};       // Alert events (Realtime)
    std::atomic<uint64_t> total_events_errors{0};        // Processing errors
    std::atomic<uint64_t> total_events_skipped{0};       // Duplicates/idempotent skips (Transactional)
    std::atomic<uint64_t> total_retries{0};              // Retry attempts (Transactional)
    
    // EventBus-level metrics
    std::atomic<uint64_t> total_events_enqueued{0};      // Events added to queue
    std::atomic<uint64_t> total_events_dequeued{0};      // Events removed from queue
    std::atomic<uint64_t> total_events_blocked{0};       // Producer blocks (BLOCK_PRODUCER policy)
    std::atomic<uint64_t> total_overflow_drops{0};       // Drops due to overflow (DROP_OLD, DROP_NEW)
    
    // Latency and timing (in nanoseconds)
    std::atomic<uint64_t> total_processing_time_ns{0};   // Cumulative processing time
    std::atomic<uint64_t> max_processing_time_ns{0};     // Maximum processing time
    std::atomic<uint64_t> event_count_for_avg{0};        // For calculating averages
    
    // Health & Status tracking (lock-free, for CONTROL PLANE)
    std::atomic<uint64_t> last_event_timestamp_ms{0};    // Last event time (stale detection)
    std::atomic<uint64_t> current_queue_depth{0};        // Current queue items
    std::atomic<uint8_t> health_status{0};               // 0=HEALTHY, 1=DEGRADED, 2=UNHEALTHY
};

enum class HealthStatus : uint8_t {
    HEALTHY = 0,
    DEGRADED = 1,
    UNHEALTHY = 2
};

/**
 * CONTROL PLANE: Non-atomic snapshot for consistent metric reads
 * Created from Metrics atomics - no locks required during creation
 * Used for health monitoring and admin decisions
 */
struct MetricSnapshot {
    // Processor metrics (snapshot)
    uint64_t total_events_processed;
    uint64_t total_events_dropped;
    uint64_t total_events_alerted;
    uint64_t total_events_errors;
    uint64_t total_events_skipped;
    uint64_t total_retries;
    
    // EventBus metrics (snapshot)
    uint64_t total_events_enqueued;
    uint64_t total_events_dequeued;
    uint64_t total_events_blocked;
    uint64_t total_overflow_drops;
    
    // Latency (snapshot)
    uint64_t total_processing_time_ns;
    uint64_t max_processing_time_ns;
    uint64_t event_count_for_avg;
    
    // Health & status (snapshot)
    uint64_t last_event_timestamp_ms;
    uint64_t current_queue_depth;
    HealthStatus health_status;
    
    // Computed fields (no atomics involved)
    uint64_t get_avg_latency_ns() const {
        return event_count_for_avg > 0 ? total_processing_time_ns / event_count_for_avg : 0;
    }
    
    uint64_t get_drop_rate_percent() const {
        uint64_t total = total_events_processed + total_events_dropped;
        return total > 0 ? (total_events_dropped * 100) / total : 0;
    }
    
    bool is_stale(uint64_t stale_threshold_ms = 10000, uint64_t now_ms = 0) const {
        if (now_ms == 0) {
            now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()
            ).count();
        }
        return (now_ms - last_event_timestamp_ms) > stale_threshold_ms;
    }
};




