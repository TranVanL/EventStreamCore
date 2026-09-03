#pragma once
#include <eventstream/core/events/event.hpp>
#include <eventstream/core/events/dead_letter_queue.hpp>
#include <eventstream/core/memory/numa.hpp>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <optional>
#include <memory>
#include <eventstream/core/queues/spsc.hpp>
#include <eventstream/core/metrics/registry.hpp>


namespace EventStream {

// Maximum number of events to drop in a single batch operation.
constexpr size_t DROP_BATCH_SIZE = 64;

class EventBusMulti {
public:

    // Use QueueID to identify which queue that dispatcher will push event to and which queue that processor will pop event from
    enum class QueueId : int { REALTIME = 0, TRANSACTIONAL = 1, BATCH = 2};

    // Particular policy for each queue when it is full 
    enum class OverflowPolicy : int { DROP_OLD = 0 , BLOCK_PRODUCER = 1 , DROP_NEW = 2 };

    // Status of the queue , used for monitoring and alerting
    enum class PressureLevel : int { NORMAL = 0 , HIGH = 1 , CRITICAL = 2 };

    EventBusMulti();
    ~EventBusMulti() = default;

    bool push(QueueId q, const EventPtr& evt);

    // Use optional to return nullptr when event is not available within the timeout period, allowing the caller to handle the absence of an event gracefully.
    std::optional<EventPtr> pop(QueueId q, std::chrono::milliseconds timeout);

    size_t size(QueueId q) const;


    // Get realtime level of pressure , used for monitoring and alerting 
    PressureLevel getRealtimePressure() const {
        return RealtimeBus_.pressure.load(std::memory_order_relaxed);
    }

    /**
     * @brief Batch drop events from a queue to DLQ
     * Drops up to DROP_BATCH_SIZE events at once
     * @param q Queue ID to drop from
     * @return Number of events dropped
     */
    size_t dropBatchFromQueue(QueueId q);

    /**
     * @brief Get reference to DeadLetterQueue
     */
    DeadLetterQueue& getDLQ() { return dlq_; }

    /**
     * @brief Set NUMA node for this bus (thread affinity)
     * @param numa_node NUMA node ID (-1 to disable)
     */
    void setNUMANode(int numa_node) { numa_node_ = numa_node; }

    /**
     * @brief Get current NUMA node binding
     */
    int getNUMANode() const { return numa_node_; }
    
private:


    // Realtime Queue use in SPSC ring buffer for high throughput and low latency , and use atomic variable to monitor pressure level
    struct RealtimeQueue {
        // Set size of ring buffer , policy drop old for real time , and set initial pressure level to normal
        SpscRingBuffer<EventPtr, 16384> ringBuffer; 
        OverflowPolicy policy = OverflowPolicy::DROP_OLD;
        std::atomic<PressureLevel> pressure{PressureLevel::NORMAL};
    };

    // Struct Q for other two queues , use mutex and condition variable to implement blocking queue , and use deque to store events
    struct Q {
        mutable std::mutex m;
        // Combine mutex and condition variable to implemnt queue in multi-threading environment , and use deque to store events
        std::condition_variable cv;
        std::deque<EventPtr> dq;
        size_t capacity = 0;
        OverflowPolicy policy;
    };


    // Declare three queues for different types of events, and a dead letter queue for handling failed events
    RealtimeQueue RealtimeBus_;
    Q TransactionalBus_;
    Q BatchBus_;
    DeadLetterQueue dlq_;
    int numa_node_ = -1;  // NUMA node binding (-1 = no binding)
   
    Q* getQueue(QueueId q) const;
};

} // namespace EventStream


