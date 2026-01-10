#pragma once
#include "Event.hpp"
#include "EventBusMulti.hpp"
#include "Topic_table.hpp"
#include "control/PipelineState.hpp"
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

private:
    EventBusMulti& event_bus_;
    PipelineStateManager* pipeline_state_;  // Non-owned reference, set by Admin

    std::deque<EventPtr> inbound_queue_;
    std::mutex inbound_mutex_;
    std::condition_variable inbound_cv_;
    size_t inbound_capacity_ = 65536;  
   
    void DispatchLoop();
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    std::shared_ptr<TopicTable> topic_table_;
    
    void adaptToPressure(const EventPtr& evt);  
};



