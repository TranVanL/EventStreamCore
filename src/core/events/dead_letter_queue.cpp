#include <eventstream/core/events/dead_letter_queue.hpp>

namespace EventStream {

DeadLetterQueue::DeadLetterQueue() {
    spdlog::info("[DeadLetterQueue] Initialized (max stored: {})", MAX_STORED_EVENTS);
}

void DeadLetterQueue::push(const Event& e) {
    total_dropped_.fetch_add(1, std::memory_order_relaxed);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Ring buffer: remove oldest if at capacity
        if (stored_events_.size() >= MAX_STORED_EVENTS) {
            stored_events_.pop_front();
        }
        stored_events_.push_back(e);
    }
    
    spdlog::warn("[DLQ] Dropped event id={} topic={} priority={} (total: {})", 
                 e.header.id, e.topic, static_cast<int>(e.header.priority),
                 total_dropped_.load(std::memory_order_relaxed));
}

void DeadLetterQueue::pushBatch(const std::vector<EventPtr>& events) {
    if (events.empty()) return;
    
    total_dropped_.fetch_add(events.size(), std::memory_order_relaxed);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (const auto& evt : events) {
            if (!evt) continue;
            
            // Ring buffer: remove oldest if at capacity
            if (stored_events_.size() >= MAX_STORED_EVENTS) {
                stored_events_.pop_front();
            }
            stored_events_.push_back(*evt);
        }
    }
    
    spdlog::warn("[DLQ] Dropped batch of {} events (total: {})", 
                 events.size(), total_dropped_.load(std::memory_order_relaxed));
}

std::vector<Event> DeadLetterQueue::getRecentEvents(size_t max_count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Event> result;
    size_t count = std::min(max_count, stored_events_.size());
    result.reserve(count);
    
    // Return newest first (reverse order)
    auto it = stored_events_.rbegin();
    for (size_t i = 0; i < count && it != stored_events_.rend(); ++i, ++it) {
        result.push_back(*it);
    }
    
    return result;
}

void DeadLetterQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    stored_events_.clear();
    spdlog::info("[DLQ] Buffer cleared (total dropped remains: {})", 
                 total_dropped_.load(std::memory_order_relaxed));
}

} // namespace EventStream
