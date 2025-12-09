#pragma once
#include "Event.hpp"
#include "Types.hpp"
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <optional>

namespace EventStream {

// Multi-queue EventBus. Minimalist, thread-safe, bounded queues per logical destination.
class EventBusMulti {
public:
    enum class QueueId : int { MAIN = 0, LOG = 1, ALERT = 2, AUDIT = 3, CUSTOM = 4 };

    struct Config {
        size_t main_capacity = 16384;
        size_t log_capacity = 8192;
        size_t alert_capacity = 2048;
        size_t audit_capacity = 2048;
    };

    explicit EventBusMulti(const Config& cfg);
    ~EventBusMulti() = default;

    // non-blocking push; returns true if enqueued, false if dropped (due to full)
    bool push(QueueId q, const EventPtr& evt);

    // blocking pop for consumer (with timeout); returns nullopt on timeout
    std::optional<EventPtr> pop(QueueId q, std::chrono::milliseconds timeout);

    // helper: size
    size_t size(QueueId q) const;

private:
    struct Q {
        mutable std::mutex m;
        std::condition_variable cv;
        std::deque<EventPtr> dq;
        size_t capacity = 0;
    };

    Q q_main_;
    Q q_log_;
    Q q_alert_;
    Q q_audit_;
    Q q_custom_;
    Q* pickQueue(QueueId q);
};

} // namespace EventStream
