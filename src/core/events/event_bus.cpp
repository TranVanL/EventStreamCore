#include <eventstream/core/events/event_bus.hpp>
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
    // Note: getQueue() always returns valid pointer (defaults to TRANSACTIONAL)
    std::lock_guard<std::mutex> lock(queue->m);
    return queue->dq.size();
}

bool EventBusMulti::push(QueueId q, const EventPtr& evt) {
   
    static thread_local auto& metrics = MetricRegistry::getInstance().getMetrics(MetricNames::EVENTBUS);
    
    // REALTIME queue uses lock-free RingBuffer
    if (q == QueueId::REALTIME) {
        // Calculate pressure level and update queue depth metric
        size_t used = RealtimeBus_.ringBuffer.SizeUsed();
        metrics.current_queue_depth.store(used, std::memory_order_relaxed);
        
        if (used >= 14000)
            RealtimeBus_.pressure.store(PressureLevel::CRITICAL, std::memory_order_relaxed);
        else if (used >= 12000)
            RealtimeBus_.pressure.store(PressureLevel::HIGH, std::memory_order_relaxed);
        else
            RealtimeBus_.pressure.store(PressureLevel::NORMAL, std::memory_order_relaxed);
        if (RealtimeBus_.ringBuffer.push(evt)) {
            metrics.total_events_processed.fetch_add(1, std::memory_order_relaxed);
            MetricRegistry::getInstance().updateEventTimestamp("EventBusMulti");
            return true;
        } else {
            // RingBuffer full - apply DROP_OLD policy
            if (RealtimeBus_.policy == OverflowPolicy::DROP_OLD) {
                // Try to make space by popping oldest
                auto old_evt = RealtimeBus_.ringBuffer.pop();
                if (old_evt) {
                    // Push dropped event to DLQ before dropping
                    dlq_.push(*old_evt.value());
                    metrics.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
                    spdlog::warn("[EventBusMulti] REALTIME OVERFLOW: Dropped oldest event to DLQ");
                }
                // Try push again
                if (RealtimeBus_.ringBuffer.push(evt)) {
                    metrics.total_events_processed.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
            // Also push incoming event to DLQ if we couldn't push it
            dlq_.push(*evt);
            metrics.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("[EventBusMulti] REALTIME OVERFLOW: Dropped incoming event id={} to DLQ", evt->header.id);
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
                // CRITICAL FIX: Add timeout to prevent infinite blocking
                // This prevents head-of-line blocking where REALTIME events
                // get stuck because Dispatcher is blocked waiting for TRANSACTIONAL queue
                // Timeout: 100ms - long enough for transactional correctness,
                // short enough to not starve other queues
                if (!queue->cv.wait_for(lock, std::chrono::milliseconds(100), 
                    [&]() { return queue->dq.size() < queue->capacity; })) {
                    // Timeout expired - return false to trigger retry/backpressure in Dispatcher
                    spdlog::warn("[EventBusMulti] TRANSACTIONAL queue full, timeout after 100ms for event id={}",
                                 evt->header.id);
                    return false;
                }
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
        metrics.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        MetricRegistry::getInstance().updateEventTimestamp("EventBusMulti");
    }
    queue->cv.notify_one();
    return true;
}
std::optional<EventPtr> EventBusMulti::pop(QueueId q, std::chrono::milliseconds timeout) {
    // Bind consumer thread to NUMA node on first pop (lazy binding)
    static thread_local bool bound = false;
    if (!bound && numa_node_ >= 0) {
        NUMABinding::bindThreadToNUMANode(numa_node_);
        bound = true;
    }

    // REALTIME queue uses lock-free RingBuffer (no locks needed)
    if (q == QueueId::REALTIME) {
        auto evt_opt = RealtimeBus_.ringBuffer.pop();
        if (evt_opt) {
            evt_opt.value()->dequeue_time_ns = EventStream::nowNs();
            return evt_opt;
        }
        return std::nullopt;
    }
    
    // TRANSACTIONAL and BATCH queues
    Q* queue = getQueue(q);
    if (queue == nullptr)
        return std::nullopt;

    std::unique_lock<std::mutex> lock(queue->m);
    // First check without waiting - fast path for available events
    if (!queue->dq.empty()) {
        EventPtr event = queue->dq.front();
        queue->dq.pop_front();
        event->dequeue_time_ns = EventStream::nowNs();
        lock.unlock();
        return event;
    }
    
    // Slow path: wait for event with timeout
    if (!queue->cv.wait_for(lock, timeout, [queue] { return !queue->dq.empty(); })) {
        return std::nullopt;
    }

    EventPtr event = queue->dq.front();
    queue->dq.pop_front();
    event->dequeue_time_ns = EventStream::nowNs();
    lock.unlock();
    return event;
}

size_t EventBusMulti::dropBatchFromQueue(QueueId q) {
    auto& metrics = MetricRegistry::getInstance().getMetrics(MetricNames::EVENTBUS);
    
    // REALTIME queue - lock-free drop from ring buffer
    if (q == QueueId::REALTIME) {
        std::vector<EventPtr> batch;
        batch.reserve(DROP_BATCH_SIZE);
        
        for (size_t i = 0; i < DROP_BATCH_SIZE; ++i) {
            auto evt = RealtimeBus_.ringBuffer.pop();
            if (!evt) break;
            batch.push_back(evt.value());
        }
        
        size_t dropped = batch.size();
        if (dropped > 0) {
            // Push dropped events to DLQ (consistent with TRANS/BATCH behavior)
            dlq_.pushBatch(batch);
            metrics.total_events_dropped.fetch_add(dropped, std::memory_order_relaxed);
            spdlog::warn("[EventBusMulti] Dropped batch of {} events from REALTIME queue to DLQ", dropped);
        }
        return dropped;
    }
    
    // TRANSACTIONAL and BATCH queues - mutex-protected deque
    Q* queue = getQueue(q);
    if (queue == nullptr)
        return 0;
    
    // Day 39 Optimization: Avoid vector allocation for small batches
    // Pre-allocate small buffer on stack, only allocate heap for large drops
    std::vector<EventPtr> batch;
    batch.reserve(std::min(DROP_BATCH_SIZE, size_t(64)));  // Pre-allocate for typical case
    
    {
        std::unique_lock<std::mutex> lock(queue->m);
        size_t to_drop = std::min(DROP_BATCH_SIZE, queue->dq.size());
        
        for (size_t i = 0; i < to_drop; ++i) {
            if (!queue->dq.empty()) {
                batch.push_back(queue->dq.front());
                queue->dq.pop_front();
            }
        }
    }
    
    size_t dropped = batch.size();
    if (dropped > 0) {
        // Push dropped batch to DLQ
        dlq_.pushBatch(batch);
        
        // Update metrics once for the batch
        metrics.total_events_dropped.fetch_add(dropped, std::memory_order_relaxed);
        
        spdlog::warn("[EventBusMulti] Dropped batch of {} events from queue {}", 
                     dropped, static_cast<int>(q));
    }
    
    return dropped;
}

} // namespace EventStream


