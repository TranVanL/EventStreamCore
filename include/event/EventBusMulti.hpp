#pragma once
#include "Event.hpp"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <optional>
#include <memory>
#include "utils/spsc_ringBuffer.hpp"
#include "metrics/metricRegistry.hpp"


namespace EventStream {

class EventBusMulti {
public:
    enum class QueueId : int { REALTIME = 0, TRANSACTIONAL = 1, BATCH = 2};
    
    enum class OverflowPolicy : int { DROP_OLD = 0 , BLOCK_PRODUCER = 1 , DROP_NEW = 2 };

    enum class PressureLevel : int { NORMAL = 0 , HIGH = 1 , CRITICAL = 2 };

    EventBusMulti() {
        TransactionalBus_.capacity = 131072; // Default queue (most events)
        TransactionalBus_.policy = OverflowPolicy::BLOCK_PRODUCER;
        BatchBus_.capacity = 32768;      
        BatchBus_.policy = OverflowPolicy::DROP_NEW; // Low-priority batch queue
    }


    ~EventBusMulti() = default;

    bool push(QueueId q, const EventPtr& evt);

    std::optional<EventPtr> pop(QueueId q, std::chrono::milliseconds timeout);

    size_t size(QueueId q) const;

    PressureLevel getRealtimePressure() const {
        return RealtimeBus_.pressure.load(std::memory_order_relaxed);
    }
private:

    struct RealtimeQueue {
        SpscRingBuffer<EventPtr, 16384> ringBuffer; 
        OverflowPolicy policy = OverflowPolicy::DROP_OLD;
        std::atomic<PressureLevel> pressure{PressureLevel::NORMAL};
    };

    struct Q {
        mutable std::mutex m;
        std::condition_variable cv;
        std::deque<EventPtr> dq;
        size_t capacity = 0;
        OverflowPolicy policy;
    };

    RealtimeQueue RealtimeBus_;
    Q TransactionalBus_;
    Q BatchBus_;
   
    Q* getQueue(QueueId q) const;
};

} // namespace EventStream


