#pragma once
#include <eventstream/core/events/event.hpp>
#include <eventstream/core/events/event_bus.hpp>
#include <eventstream/core/events/topic_table.hpp>
#include <eventstream/core/control/pipeline_state.hpp>
#include <eventstream/core/queues/mpsc_queue.hpp>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <optional>
#include <spdlog/spdlog.h>

using namespace EventStream;

class Dispatcher {
public:
    explicit Dispatcher(EventBusMulti& bus, PipelineStateManager* pipeline_state = nullptr) 
        : event_bus_(bus), pipeline_state_(pipeline_state) {}
    ~Dispatcher() noexcept;

    // lifecycle
    void start();
    void stop();

    bool tryPush(const EventPtr& evt);
    std::optional<EventPtr> tryPop(std::chrono::milliseconds timeout);

    EventBusMulti::QueueId Route(const EventPtr& evt);

    void setTopicTable(std::shared_ptr<TopicTable> t) {
        topic_table_ = std::move(t);
    }
    void setPipelineState(PipelineStateManager* state) {
        pipeline_state_ = state;
        spdlog::info("[Dispatcher] Pipeline state manager connected");
    }

private:
    EventBusMulti& event_bus_;
    PipelineStateManager* pipeline_state_;  // Non-owned reference, set by Admin

    // Lock-free MPSC (Multi-Producer Single-Consumer) queue for inbound events
    // Thread-safe for multiple TCP/UDP ingest threads pushing concurrently
    // Single consumer: Dispatcher DispatchLoop
    // Capacity: 65536 events (configurable via template parameter)
    MpscQueue<EventPtr, 65536> inbound_queue_;
   
    void DispatchLoop();
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    std::shared_ptr<TopicTable> topic_table_;
    
    void adaptToPressure(const EventPtr& evt);  
};



