#pragma once

#include <cstdint>
#include <vector>
#include <cstring>

namespace eventstream::core {

/**
 * HighPerformanceEvent - Optimized event for lock-free SPSC queue
 * 
 * Design decisions:
 * 1. alignas(64) - Cache-line aligned to prevent false sharing
 * 2. Fixed-size payload (512 bytes) - Predictable memory access
 * 3. No virtual functions - No vptr overhead
 * 4. Timing fields - For latency measurement without extra allocation
 * 5. Minimal fields - Only what's critical for fast path
 * 
 * Memory layout (64-byte aligned):
 * Offset  Size  Field
 * ------  ----  -----
 * 0       8     event_id
 * 8       8     ingest_timestamp_ns
 * 16      8     dequeue_timestamp_ns
 * 24      8     process_done_timestamp_ns
 * 32      4     topic_id
 * 36      4     payload_size
 * 40      4     source_type
 * 44      20    padding/reserved
 * 64      512   payload (fixed size)
 * ------
 * Total: 576 bytes (9 cache lines)
 */
struct alignas(64) HighPerformanceEvent {
    // Event identification & tracking (24 bytes)
    uint64_t event_id;                    // Unique event identifier for dedup
    uint32_t topic_id;                    // Topic/channel identifier
    
    // Timing information for latency measurement (24 bytes)
    uint64_t ingest_timestamp_ns;         // When event entered producer
    uint64_t dequeue_timestamp_ns;        // When consumer dequeued
    uint64_t process_done_timestamp_ns;   // When processing completed
    
    // Metadata (8 bytes)
    uint32_t payload_size;                // Actual bytes in payload (0-512)
    uint32_t source_type;                 // Source: TCP=0, UDP=1, FILE=2, etc.
    
    // Fixed-size payload (512 bytes) - cache-friendly
    // Avoids dynamic allocation and improves cache locality
    static constexpr size_t PAYLOAD_SIZE = 512;
    uint8_t payload[PAYLOAD_SIZE];
    
    /**
     * Initialize event to clean state
     */
    void reset() {
        event_id = 0;
        topic_id = 0;
        ingest_timestamp_ns = 0;
        dequeue_timestamp_ns = 0;
        process_done_timestamp_ns = 0;
        payload_size = 0;
        source_type = 0;
        std::memset(payload, 0, PAYLOAD_SIZE);
    }
    
    /**
     * Get latency from ingest to dequeue (consumer pickup time)
     * @return Nanoseconds
     */
    uint64_t ingest_to_dequeue_ns() const {
        return dequeue_timestamp_ns - ingest_timestamp_ns;
    }
    
    /**
     * Get latency from dequeue to processing done (processing time)
     * @return Nanoseconds
     */
    uint64_t dequeue_to_done_ns() const {
        return process_done_timestamp_ns - dequeue_timestamp_ns;
    }
    
    /**
     * Get total latency from ingest to done
     * @return Nanoseconds
     */
    uint64_t total_latency_ns() const {
        return process_done_timestamp_ns - ingest_timestamp_ns;
    }
};

// Verify alignment and size
static_assert(alignof(HighPerformanceEvent) == 64, 
              "HighPerformanceEvent must be 64-byte aligned");
static_assert(sizeof(HighPerformanceEvent) == 576,
              "HighPerformanceEvent should be 576 bytes (9 cache lines)");

} // namespace eventstream::core
