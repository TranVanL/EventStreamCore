#pragma once

#include <eventstream/core/events/event.hpp>
#include <vector>
#include <atomic>
#include <spdlog/spdlog.h>

namespace EventStream {

/**
 * @class DeadLetterQueue
 * @brief Semantic interface for handling dropped events.
 *
 * Stores events that have been dropped due to:
 * - Queue overflow (CRITICAL capacity reached)
 * - DROPPING state (intentional drop during backpressure recovery)
 * - Control plane actions (dropBatchEvents)
 *
 * Day 22: Semantic DLQ only - no persistence, no retry logic.
 * Future: Extend to include persistent storage and retry policies.
 */
class DeadLetterQueue {
public:
    DeadLetterQueue();
    ~DeadLetterQueue() = default;

    /**
     * @brief Push dropped event to DLQ
     * @param e The event that was dropped
     */
    void push(const Event& e);

    /**
     * @brief Push batch of dropped events
     * @param events Vector of events to store in DLQ
     */
    void pushBatch(const std::vector<EventPtr>& events);

    /**
     * @brief Get total count of events in DLQ
     * @return Number of dropped events stored
     */
    size_t size() const {
        return total_dropped_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<size_t> total_dropped_{0};
    
    // Day 22: Simple in-memory tracking only
    // Future: Add persistent storage (database, file log, etc.)
};

} // namespace EventStream
