#include "event/EventBusMulti.hpp"
#include <chrono>

using namespace EventStream;


Q* EventBusMulti::getQueue(QueueId q){
    switch(q){
        case QueueId::REALTIME:
            return &RealtimeBus_;
        case QueueId::TRANSACTIONAL:
            return &TransactionalBus_;
        case QueueId::BATCH:
            return &BatchBus_;
        default:
            return &TransactionalBus_;
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
    if(queue == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(queue->m);
        if(queue->dq.size() >= queue->capacity){
            return false; 
        }
        queue->dq.push_back(evt);
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
   return event;
}
