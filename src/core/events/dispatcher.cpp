#include <eventstream/core/events/dispatcher.hpp>
#include <algorithm>
#include <cassert>

Dispatcher::~Dispatcher() noexcept {
    spdlog::info("[DESTRUCTOR] Dispatcher being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] Dispatcher destroyed successfully");
}

void Dispatcher::start() {
    running_.store(true,std::memory_order_release);
    worker_thread_ = std::thread(&Dispatcher::dispatchLoop,this);
    spdlog::info("Dispatcher started.");
}

void Dispatcher::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    spdlog::info("Dispatcher stopped.");
}

bool Dispatcher::tryPush(const EventStream::EventPtr& evt) {
    if (!inbound_queue_.push(evt)) {
        spdlog::warn("[BACKPRESSURE] Dispatcher MPSC queue full, dropping event");
        return false;
    }
    return true;
}

std::optional<EventStream::EventPtr> Dispatcher::tryPop(std::chrono::milliseconds timeout) {
    auto evt_opt = inbound_queue_.pop();
    if (!evt_opt) {
        return std::nullopt;
    }
    return evt_opt;
}

EventStream::EventBusMulti::QueueId Dispatcher::route(const EventStream::EventPtr& evt) {
    // Caller must validate evt != nullptr before calling route()
    assert(evt != nullptr && "route() called with null event - caller bug");
    
    if (!evt) {
        spdlog::error("CRITICAL: Null event pointer in route - this is a caller bug!");
        return EventStream::EventBusMulti::QueueId::TRANSACTIONAL;
    }

    EventStream::EventBusMulti::QueueId queueId;
    EventStream::EventPriority priority = EventStream::EventPriority::MEDIUM;

    // If topic table exists and the topic is found, update priority from table
    if (topic_table_ && topic_table_->findTopic(evt->topic, priority)) {
        spdlog::info("Found topic {} with priority {}", evt->topic, static_cast<int>(priority));
    }

    // Only upgrade priority if table priority is higher than client priority
    if (evt->header.priority < priority) {
        spdlog::debug("Upgrading event {} priority from {} to {}", evt->header.id, 
                      static_cast<int>(evt->header.priority), static_cast<int>(priority));
        evt->header.priority = priority;
    }

    // Adaptive pressure handling: downgrade priority if system is under high pressure
    adaptToPressure(evt);

    // Route based on final priority
    if (evt->header.priority == EventStream::EventPriority::CRITICAL || 
        evt->header.priority == EventStream::EventPriority::HIGH) {
        queueId = EventStream::EventBusMulti::QueueId::REALTIME;
    }
    else if (evt->header.priority == EventStream::EventPriority::MEDIUM || 
             evt->header.priority == EventStream::EventPriority::LOW) {
        queueId = EventStream::EventBusMulti::QueueId::TRANSACTIONAL;
    }
    else {
        queueId = EventStream::EventBusMulti::QueueId::BATCH;
    }

    return queueId;
}

void Dispatcher::dispatchLoop(){
    spdlog::info("Dispatcher dispatch loop started.");

    while (running_.load(std::memory_order_acquire))
    {  
        auto event_opt = inbound_queue_.pop();
        if (!event_opt) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        
        if (!event_opt.value()) {
            spdlog::error("[Dispatcher] Received null event from queue - discarding");
            continue;
        }
        
        auto queueId = route(event_opt.value());
        
        // Backpressure: retry with exponential backoff if EventBus is full
        bool pushed = false;
        int retry_count = 0;
        constexpr int MAX_RETRIES = 3;
        
        while (!pushed && retry_count < MAX_RETRIES) {
            pushed = event_bus_.push(queueId, event_opt.value());
            
            if (!pushed) {
                retry_count++;
                if (retry_count < MAX_RETRIES) {
                    auto backoff_us = 10 * (1 << (retry_count - 1));
                    std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                    spdlog::debug("[BACKPRESSURE] EventBus queue {} full, retry {}/{} for event {}",
                                 static_cast<int>(queueId), retry_count, MAX_RETRIES, 
                                 event_opt.value()->header.id);
                }
            }
        }
        
        if (!pushed) {
            event_bus_.getDLQ().push(*event_opt.value());
            spdlog::warn("[BACKPRESSURE] Failed to push event {} to queue {} after {} retries. Pushed to DLQ.",
                        event_opt.value()->header.id, static_cast<int>(queueId), MAX_RETRIES);
        }
    }
    
    spdlog::info("Dispatcher dispatch loop stopped.");
}

void Dispatcher::adaptToPressure(const EventStream::EventPtr& evt) {
    if (!evt) return;
    
    auto rt_pressure = event_bus_.getRealtimePressure();
    
    if (rt_pressure == EventStream::EventBusMulti::PressureLevel::CRITICAL) {
        if (evt->header.priority == EventStream::EventPriority::HIGH) {
            spdlog::debug("System CRITICAL pressure: Downgrading HIGH priority event {} to MEDIUM", evt->header.id);
            evt->header.priority = EventStream::EventPriority::MEDIUM;
        }
    } 
    else if (rt_pressure == EventStream::EventBusMulti::PressureLevel::HIGH) {
        if (evt->header.priority == EventStream::EventPriority::HIGH) {
            spdlog::debug("System HIGH pressure: Downgrading HIGH priority event {} to MEDIUM", evt->header.id);
            evt->header.priority = EventStream::EventPriority::MEDIUM;
        }
    }
}


