#include "event/EventBusMulti.hpp"
#include <condition_variable>
#include <chrono>
#include <spdlog/spdlog.h>

namespace EventStream {

EventBusMulti::EventBusMulti() {
    TransactionalBus_.capacity = 131072;
    TransactionalBus_.policy = OverflowPolicy::BLOCK_PRODUCER;
    BatchBus_.capacity = 32768;
    BatchBus_.policy = OverflowPolicy::DROP_NEW;
}

EventBusMulti::Q* EventBusMulti::getQueue(QueueId q) const {
    switch (q) {
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
    if (queue == nullptr)
        return 0;
    std::lock_guard<std::mutex> lock(queue->m);
    return queue->dq.size();
}

bool EventBusMulti::push(QueueId q, const EventPtr& evt) {
    auto& metrics = MetricRegistry::getInstance().getMetrics("EventBusMulti"); 
    
    // REALTIME queue uses lock-free RingBuffer
    if (q == QueueId::REALTIME) {
        // Calculate pressure level
        size_t used = RealtimeBus_.ringBuffer.SizeUsed();
        if (used >= 14000)
            RealtimeBus_.pressure.store(PressureLevel::CRITICAL, std::memory_order_relaxed);
        else if (used >= 12000)
            RealtimeBus_.pressure.store(PressureLevel::HIGH, std::memory_order_relaxed);
        else
            RealtimeBus_.pressure.store(PressureLevel::NORMAL, std::memory_order_relaxed);
        if (RealtimeBus_.ringBuffer.push(evt)) {
            metrics.total_events_enqueued.fetch_add(1, std::memory_order_relaxed);
            MetricRegistry::getInstance().updateEventTimestamp("EventBusMulti");
            return true;
        } else {
            // RingBuffer full - apply DROP_OLD policy
            if (RealtimeBus_.policy == OverflowPolicy::DROP_OLD) {
                // Try to make space by popping oldest
                auto old_evt = RealtimeBus_.ringBuffer.pop();
                if (old_evt) {
                    metrics.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
                    spdlog::warn("[EventBusMulti] REALTIME OVERFLOW: Dropped oldest event");
                }
                // Try push again
                if (RealtimeBus_.ringBuffer.push(evt)) {
                    metrics.total_events_enqueued.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
            metrics.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("[EventBusMulti] REALTIME OVERFLOW: Dropped incoming event id={}", evt->header.id);
            return false;
        }
    }
    
    // TRANSACTIONAL and BATCH queues use mutex-protected deque
    Q* queue = getQueue(q);
    if (queue == nullptr)
        return false;
    {
        std::unique_lock<std::mutex> lock(queue->m);
        if (queue->dq.size() >= queue->capacity) {
            switch (queue->policy) {    
            case OverflowPolicy::BLOCK_PRODUCER:
                queue->cv.wait(lock, [&]() { return queue->dq.size() < queue->capacity; });
                metrics.total_events_blocked.fetch_add(1, std::memory_order_relaxed);
                break;
            case OverflowPolicy::DROP_NEW:
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
        MetricRegistry::getInstance().updateEventTimestamp("EventBusMulti");
    }
    queue->cv.notify_one();
    return true;
}

std::optional<EventPtr> EventBusMulti::pop(QueueId q, std::chrono::milliseconds timeout) {
    // REALTIME queue uses lock-free RingBuffer
    if (q == QueueId::REALTIME) {
        auto evt = RealtimeBus_.ringBuffer.pop();
        if (evt) {
            auto& metrics = MetricRegistry::getInstance().getMetrics("EventBusMulti");
            metrics.total_events_dequeued.fetch_add(1, std::memory_order_relaxed);
            return evt;
        }
        return std::nullopt;
    }
    
    // TRANSACTIONAL and BATCH queues
    Q* queue = getQueue(q);
    if (queue == nullptr)
        return std::nullopt;

    std::unique_lock<std::mutex> lock(queue->m);
    if (!queue->cv.wait_for(lock, timeout, [queue] { return !queue->dq.empty(); })) {
        return std::nullopt;
    }

    EventPtr event = queue->dq.front();
    queue->dq.pop_front();

    auto& metrics = MetricRegistry::getInstance().getMetrics("EventBusMulti");
    metrics.total_events_dequeued.fetch_add(1, std::memory_order_relaxed);

    return event;
}
} // namespace EventStream

