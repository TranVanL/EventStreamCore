#pragma once
#include "Event.hpp"
#include "Types.hpp"
#include "EventBusMulti.hpp"
#include "Topic_table.hpp"
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <optional>

namespace EventStream {

class Dispatcher {
public:
    struct Config {
        size_t inbound_capacity = 8192;
    };

    explicit Dispatcher(EventBusMulti* bus, const Config& cfg);
    ~Dispatcher();

    // enqueue from parser (non-blocking); return false if inbound is full
    bool enqueue(const EventPtr& evt);

    // lifecycle
    void start();
    void stop();

    // set topic table for override
    void setTopicTable(std::shared_ptr<TopicTable> t) { topic_table_ = std::move(t); }

    // custom routing function override (optional)
    // routing function returns list of EventBusMulti::QueueId to push
    using RoutingFn = std::function<std::vector<EventBusMulti::QueueId>(const EventPtr&)>;
    void setRoutingFn(RoutingFn fn) { std::lock_guard lk(routing_mtx_); routing_fn_ = std::move(fn); }

private:
    void workerLoop();
    std::vector<EventBusMulti::QueueId> defaultRoute(const EventPtr& evt);

    // inbound queue (simple bounded deque)
    bool inboundTryPush(const EventPtr& evt);
    std::optional<EventPtr> inboundPop(std::chrono::milliseconds timeout);

    EventBusMulti* bus_;
    std::shared_ptr<TopicTable> topic_table_;

    // inbound
    mutable std::mutex inbound_mtx_;
    std::condition_variable inbound_cv_;
    std::deque<EventPtr> inbound_q_;
    size_t inbound_capacity_;

    // worker
    std::thread worker_;
    std::atomic<bool> running_{false};

    // routing override
    RoutingFn routing_fn_;
    mutable std::mutex routing_mtx_;
};
}
