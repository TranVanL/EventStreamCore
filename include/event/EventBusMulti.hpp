#pragma once
#include "Event.hpp"
#include "DeadLetterQueue.hpp"
#include "core/numa_binding.hpp"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <optional>
#include <memory>
#include "utils/spsc_ringBuffer.hpp"
#include "metrics/metricRegistry.hpp"


namespace EventStream {

// Batch drop size for DROPPING state - prepare for distributed mode
constexpr size_t DROP_BATCH_SIZE = 64;

class EventBusMulti {
public:
    enum class QueueId : int { REALTIME = 0, TRANSACTIONAL = 1, BATCH = 2};
    
    enum class OverflowPolicy : int { DROP_OLD = 0 , BLOCK_PRODUCER = 1 , DROP_NEW = 2 };

    enum class PressureLevel : int { NORMAL = 0 , HIGH = 1 , CRITICAL = 2 };

    EventBusMulti();
    ~EventBusMulti() = default;

    bool push(QueueId q, const EventPtr& evt);

    std::optional<EventPtr> pop(QueueId q, std::chrono::milliseconds timeout);

    size_t size(QueueId q) const;

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
    DeadLetterQueue dlq_;
    int numa_node_ = -1;  // NUMA node binding (-1 = no binding)
   
    Q* getQueue(QueueId q) const;
};

} // namespace EventStream


