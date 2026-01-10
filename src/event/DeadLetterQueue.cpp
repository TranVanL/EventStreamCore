#include "event/DeadLetterQueue.hpp"

namespace EventStream {

DeadLetterQueue::DeadLetterQueue() {
    spdlog::info("[DeadLetterQueue] Initialized");
}

void DeadLetterQueue::push(const Event& e) {
    total_dropped_.fetch_add(1, std::memory_order_relaxed);
    spdlog::warn("[DLQ] Dropped event id={} topic={} priority={}", 
                 e.header.id, e.topic, static_cast<int>(e.header.priority));
}

void DeadLetterQueue::pushBatch(const std::vector<EventPtr>& events) {
    if (events.empty()) return;
    
    total_dropped_.fetch_add(events.size(), std::memory_order_relaxed);
    
    spdlog::warn("[DLQ] Dropped batch of {} events", events.size());
    for (const auto& evt : events) {
        if (evt) {
            spdlog::debug("[DLQ]   - Event id={} topic={} priority={}", 
                         evt->header.id, evt->topic, static_cast<int>(evt->header.priority));
        }
    }
}

} // namespace EventStream
