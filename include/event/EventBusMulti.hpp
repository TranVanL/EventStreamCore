#pragma once
#include "Event.hpp"
#include "Types.hpp"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <optional>

using namespace EventStream;
class EventBusMulti {
public:
    enum class QueueId : int { REALTIME = 0, TRANSACTIONAL = 1, BATCH = 2};
  
    EventBusMulti() {
        RealtimeBus_.capacity = 16384;
        TransactionalBus_.capacity = 8192;
        BatchBus_.capacity = 2048;
    }
    ~EventBusMulti() = default;

    bool push(QueueId q, const EventPtr& evt);

    std::optional<EventPtr> pop(QueueId q, std::chrono::milliseconds timeout);

    size_t size(QueueId q) const;

private:
    struct Q {
        mutable std::mutex m;
        std::condition_variable cv;
        std::deque<EventPtr> dq;
        size_t capacity = 0;
    };

    Q RealtimeBus_;
    Q TransactionalBus_;
    Q BatchBus_;
   
    Q* getQueue(QueueId q);
};


