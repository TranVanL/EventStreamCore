#pragma once 
#include <atomic>
#include <cstdint>

/**
 * @brief Metrics tracking structure for processors and event bus
 * 
 * All counters are lock-free atomic for minimal overhead.
 * Memory ordering: relaxed (no synchronization overhead, acceptable for metrics)
 */
struct Metrics {
    // Processor-level metrics
    std::atomic<uint64_t> total_events_processed{0};     // Events successfully processed
    std::atomic<uint64_t> total_events_dropped{0};       // Events dropped due to SLA/errors/overflow
    std::atomic<uint64_t> total_events_alerted{0};       // Alert events (Realtime)
    std::atomic<uint64_t> total_events_errors{0};        // Processing errors
    std::atomic<uint64_t> total_events_skipped{0};       // Duplicates/idempotent skips (Transactional)
    
    // EventBus-level metrics
    std::atomic<uint64_t> total_events_enqueued{0};      // Events added to queue
    std::atomic<uint64_t> total_events_dequeued{0};      // Events removed from queue
    std::atomic<uint64_t> total_events_blocked{0};       // Producer blocks (BLOCK_PRODUCER policy)
    std::atomic<uint64_t> total_overflow_drops{0};       // Drops due to overflow (DROP_OLD, DROP_NEW)
    
    // Latency and timing (in nanoseconds)
    std::atomic<uint64_t> total_processing_time_ns{0};   // Cumulative processing time
    std::atomic<uint64_t> max_processing_time_ns{0};     // Maximum processing time
    std::atomic<uint64_t> event_count_for_avg{0};        // For calculating averages
};

