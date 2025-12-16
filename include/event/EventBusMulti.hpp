#pragma once
#include "Event.hpp"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <optional>
#include <memory>
#include "utils/spsc_ringBuffer.hpp"
#include "eventprocessor/metricRegistry.hpp"


namespace EventStream {

class EventBusMulti {
public:
    enum class QueueId : int { REALTIME = 0, TRANSACTIONAL = 1, BATCH = 2};
    
    enum class OverflowPolicy : int { DROP_OLD = 0 , BLOCK_PRODUCER = 1 , DROP_NEW = 2 };

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

private:

    struct RealtimeQueue {
        SpscRingBuffer<EventPtr, 16384> ringBuffer; // 16K capacity for low-latency
        OverflowPolicy policy = OverflowPolicy::DROP_OLD;
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


