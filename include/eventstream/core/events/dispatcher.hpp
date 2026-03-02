#pragma once
#include <eventstream/core/events/event.hpp>
#include <eventstream/core/events/event_bus.hpp>
#include <eventstream/core/events/topic_table.hpp>
#include <eventstream/core/control/pipeline_state.hpp>
#include <eventstream/core/queues/mpsc_queue.hpp>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <optional>
#include <spdlog/spdlog.h>

/**
 * @class Dispatcher
 * @brief Routes inbound events from ingest servers to the correct EventBus queue.
 *
 * Ingest threads (TCP/UDP) push events into a lock-free MPSC queue.
 * A single dispatch thread pops events, determines the target queue
 * via priority + topic table, and pushes them into EventBusMulti.
 */
class Dispatcher {
public:
    explicit Dispatcher(EventStream::EventBusMulti& bus,
                        PipelineStateManager* pipeline_state = nullptr)
        : event_bus_(bus), pipeline_state_(pipeline_state) {}
    ~Dispatcher() noexcept;

    // Lifecycle
    void start();
    void stop();

    /// Push an event into the inbound MPSC queue (thread-safe, called by ingest threads).
    bool tryPush(const EventStream::EventPtr& evt);

    /// Pop an event from the inbound queue (single-consumer).
    std::optional<EventStream::EventPtr> tryPop(std::chrono::milliseconds timeout);

    /// Determine the target EventBus queue for an event based on priority & topic.
    EventStream::EventBusMulti::QueueId route(const EventStream::EventPtr& evt);

    void setTopicTable(std::shared_ptr<EventStream::TopicTable> t) {
        topic_table_ = std::move(t);
    }

    void setPipelineState(PipelineStateManager* state) {
        pipeline_state_ = state;
        spdlog::info("[Dispatcher] Pipeline state manager connected");
    }

private:
    EventStream::EventBusMulti& event_bus_;
    PipelineStateManager* pipeline_state_ = nullptr;  ///< Non-owned, set by Admin

    /// Lock-free MPSC queue: multiple ingest threads → single dispatch thread.
    MpscQueue<EventStream::EventPtr, 65536> inbound_queue_;

    void dispatchLoop();
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    std::shared_ptr<EventStream::TopicTable> topic_table_;

    /// Downgrade event priority when the system is under pressure.
    void adaptToPressure(const EventStream::EventPtr& evt);
};



