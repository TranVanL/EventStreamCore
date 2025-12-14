#include "event/EventBusMulti.hpp"
#include <condition_variable>
#include <chrono>
#include <spdlog/spdlog.h>

namespace EventStream {


EventBusMulti::Q* EventBusMulti::getQueue(QueueId q) const {
    switch(q){
        case QueueId::REALTIME:
            return const_cast<Q*>(&RealtimeBus_);
        case QueueId::TRANSACTIONAL:
            return const_cast<Q*>(&TransactionalBus_);
        case QueueId::BATCH:
            return const_cast<Q*>(&BatchBus_);
        default:
            return const_cast<Q*>(&TransactionalBus_);
    }
}

size_t EventBusMulti::size(QueueId q) const {
    Q* queue = getQueue(q);
    if (queue == nullptr) return 0;
    std::lock_guard<std::mutex> lock(queue->m);
    return queue->dq.size();
}

bool EventBusMulti::push(QueueId q, const EventPtr& evt){
    Q* queue = getQueue(q);
    auto& metrics = MetricRegistry::getInstance().getMetrics("EventBusMulti"); 
    if(queue == nullptr) return false;
    {
        std::unique_lock<std::mutex> lock(queue->m);
        if(queue->dq.size() >= queue->capacity){
            switch (queue->policy)
            {
            case OverflowPolicy::DROP_OLD:
                // Remove oldest event and track it
                if (!queue->dq.empty()) {
                    auto old_evt = queue->dq.front();
                    queue->dq.pop_front();
                    metrics.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
                    spdlog::warn("[EventBusMulti] Queue {} OVERFLOW: Dropped oldest event id={}",
                               static_cast<int>(q), old_evt->header.id);
                }
                break;
                
            case OverflowPolicy::BLOCK_PRODUCER:
                // Wait until space is available
                queue->cv.wait(lock, [&](){ return queue->dq.size() < queue->capacity; });
                metrics.total_events_blocked.fetch_add(1, std::memory_order_relaxed);
                break;
                
            case OverflowPolicy::DROP_NEW:
                // Reject incoming event
                metrics.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
                spdlog::warn("[EventBusMulti] Queue {} OVERFLOW: Dropped incoming event id={}",
                           static_cast<int>(q), evt->header.id);
                return false;
                
            default:
                break;
            } 
        }
        queue->dq.push_back(evt);
        metrics.total_events_enqueued.fetch_add(1, std::memory_order_relaxed);
    }
    queue->cv.notify_one();
    return true;
}

std::optional<EventPtr> EventBusMulti::pop(QueueId q, std::chrono::milliseconds timeout){
    Q* queue = getQueue(q);
    if (queue == nullptr) return std::nullopt;

    std::unique_lock<std::mutex> lock(queue->m);
    if (!queue->cv.wait_for(lock, timeout, [queue] { return !queue->dq.empty(); })) {
        return std::nullopt; 
    }

   EventPtr event = queue -> dq.front();
   queue->dq.pop_front();
   
   auto& metrics = MetricRegistry::getInstance().getMetrics("EventBusMulti");
   metrics.total_events_dequeued.fetch_add(1, std::memory_order_relaxed);
   
   return event;
}

} // namespace EventStream
