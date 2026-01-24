#include "event/Dispatcher.hpp"
#include <algorithm>
#include <iostream>

using namespace EventStream;

Dispatcher::~Dispatcher() noexcept {
    spdlog::info("[DESTRUCTOR] Dispatcher being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] Dispatcher destroyed successfully");
}

void Dispatcher::start() {
    running_.store(true,std::memory_order_release);
    worker_thread_ = std::thread(&Dispatcher::DispatchLoop,this);
    spdlog::info("Dispatcher started.");
}

void Dispatcher::stop() {
    running_.store(false, std::memory_order_release);
    // No need to notify - lock-free queue doesn't have condition variable
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    spdlog::info("Dispatcher stopped.");
}

bool Dispatcher::tryPush(const EventPtr& evt) {
    // Try to push to lock-free ring buffer (no mutex)
    if (!inbound_queue_.push(evt)) {
        // Queue full
        spdlog::warn("Dispatcher inbound queue full, dropping event");
        return false;
    }
    // Signal that there are pending events
    has_pending_.store(true, std::memory_order_relaxed);
    return true;
}

std::optional<EventPtr> Dispatcher::tryPop(std::chrono::milliseconds timeout) {
    // For backward compatibility, try to pop from ring buffer
    auto evt_opt = inbound_queue_.pop();
    if (!evt_opt) {
        // Could add sleep here if needed for timeout behavior
        return std::nullopt;
    }
    return evt_opt;
}

EventBusMulti::QueueId Dispatcher::Route(const EventPtr& evt) {
    if (!evt) {
        spdlog::warn("Null event pointer in Route");
        return EventBusMulti::QueueId::TRANSACTIONAL;
    }

    EventBusMulti::QueueId queueId;
    EventPriority priority = EventPriority::MEDIUM;

    // If topic table exists and the topic is found, update priority from table
    if (topic_table_ && topic_table_->FoundTopic(evt->topic, priority)) {
        spdlog::info("Found topic {} with priority {}", evt->topic, static_cast<int>(priority));
    }

    // Priority handling logic:
    // - If topic is found in table: only upgrade if table priority is higher than client priority
    // - If topic is not found: maximum allowed priority is MEDIUM
    if (evt->header.priority < priority) {
        spdlog::debug("Upgrading event {} priority from {} to {}", evt->header.id, 
                      static_cast<int>(evt->header.priority), static_cast<int>(priority));
        evt->header.priority = priority;
    }

    // Adaptive pressure handling: downgrade priority if system is under high pressure
    adaptToPressure(evt);

    // Determine routes based on final priority
    if (evt->header.priority == EventPriority::CRITICAL || evt->header.priority == EventPriority::HIGH) {
        queueId = EventBusMulti::QueueId::REALTIME;
    }
    else if (evt->header.priority == EventPriority::MEDIUM || evt->header.priority == EventPriority::LOW) {
        queueId = EventBusMulti::QueueId::TRANSACTIONAL;
    }
    else { // BATCH
        queueId = EventBusMulti::QueueId::BATCH;
    }

    return queueId;
}

void Dispatcher::DispatchLoop(){
    spdlog::info("Dispatcher DispatchLoop started.");

    while (running_.load(std::memory_order_acquire))
    {  
        // Check pipeline state - tôn trọng quyết định của Admin
        if (pipeline_state_) {
            PipelineState state = pipeline_state_->getState();
            
            // Nếu state là PAUSED hoặc DRAINING, dispatcher không consume event
            if (state == PipelineState::PAUSED || state == PipelineState::DRAINING) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
        
        // Try to pop from lock-free ring buffer
        auto event_opt = inbound_queue_.pop();
        if (!event_opt) {
            // No event available, sleep briefly to avoid busy-wait
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        
        auto queueId = Route(event_opt.value());
        bool pushed = event_bus_.push(queueId, event_opt.value());

        if (!pushed) {
            spdlog::warn("Failed to push event {} to queue {} . Dropping event.", event_opt.value()->header.id, static_cast<int>(queueId));
        }
        
    }
    
    spdlog::info("Dispatcher DispatchLoop stopped.");
}

void Dispatcher::adaptToPressure(const EventPtr& evt) {
    if (!evt) return;
    
    auto rt_pressure = event_bus_.getRealtimePressure();
    
    if (rt_pressure == EventBusMulti::PressureLevel::CRITICAL) {
        if (evt->header.priority == EventPriority::CRITICAL || evt->header.priority == EventPriority::HIGH) {
            evt->header.priority = EventPriority::MEDIUM;
        }
    } 
    else if (rt_pressure == EventBusMulti::PressureLevel::HIGH) {
        // Only downgrade HIGH priority events when system is high pressure
        if (evt->header.priority == EventPriority::HIGH) {
            spdlog::debug("System HIGH pressure: Downgrading HIGH priority event {} to MEDIUM", evt->header.id);
            evt->header.priority = EventPriority::MEDIUM;
        }
    }
}


