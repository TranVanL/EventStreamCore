#pragma once

#include <eventstream/core/events/event.hpp>
#include <vector>
#include <deque>
#include <atomic>
#include <mutex>
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
 * Features:
 * - Tracks total dropped count (atomic)
 * - Stores recent N events for debugging (ring buffer)
 * - Thread-safe push operations
 * 
 * Future: Extend to include persistent storage and retry policies.
 */
class DeadLetterQueue {
public:
    static constexpr size_t MAX_STORED_EVENTS = 1000;  // Keep last 1000 events for debugging
    
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
     * @brief Get total count of events ever dropped
     * @return Number of dropped events (cumulative)
     */
    size_t totalDropped() const {
        return total_dropped_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Get count of currently stored events
     * @return Number of events in buffer (max MAX_STORED_EVENTS)
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stored_events_.size();
    }
    
    /**
     * @brief Get recent dropped events for debugging
     * @param max_count Maximum events to retrieve
     * @return Vector of recent dropped events (newest first)
     */
    std::vector<Event> getRecentEvents(size_t max_count = 100) const;
    
    /**
     * @brief Clear stored events (for testing or memory management)
     */
    void clear();

private:
    std::atomic<size_t> total_dropped_{0};
    mutable std::mutex mutex_;
    std::deque<Event> stored_events_;  // Ring buffer of recent events
};

} // namespace EventStream
