#include "event/EventBusMulti.hpp"
using namespace EventStream;
#include <chrono>

EventBusMulti::EventBusMulti(const Config& cfg) {
    q_main_.capacity = cfg.main_capacity;
    q_log_.capacity = cfg.log_capacity;
    q_alert_.capacity = cfg.alert_capacity;
    q_audit_.capacity = cfg.audit_capacity;
}

EventBusMulti::Q* EventBusMulti::pickQueue(QueueId q) {
    switch(q) {
        case QueueId::MAIN: return &q_main_;
        case QueueId::LOG:  return &q_log_;
        case QueueId::ALERT:return &q_alert_;
        case QueueId::AUDIT:return &q_audit_;
        default: return &q_main_;
    }
}

bool EventBusMulti::push(QueueId qid, const EventPtr& evt) {
    Q* q = pickQueue(qid);
    std::unique_lock lk(q->m);
    if (q->dq.size() >= q->capacity) {
        // drop policy: drop newest (don't enqueue). Could be changeable.
        return false;
    }
    q->dq.push_back(evt);
    lk.unlock();
    q->cv.notify_one();
    return true;
}

std::optional<EventPtr> EventBusMulti::pop(QueueId qid, std::chrono::milliseconds timeout) {
    Q* q = pickQueue(qid);
    std::unique_lock lk(q->m);
    if (q->dq.empty()) {
        if (!q->cv.wait_for(lk, timeout, [&]{ return !q->dq.empty(); })) {
            return std::nullopt;
        }
    }
    EventPtr ev = q->dq.front();
    q->dq.pop_front();
    return ev;
}

size_t EventBusMulti::size(QueueId qid) const {
    Q* q = const_cast<EventBusMulti*>(this)->pickQueue(qid);
    std::lock_guard lk(q->m);
    return q->dq.size();
}
