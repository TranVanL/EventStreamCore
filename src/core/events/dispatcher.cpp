#include <eventstream/core/events/dispatcher.hpp>
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
    // Thread-safe push to MPSC queue (multiple producers allowed)
    if (!inbound_queue_.push(evt)) {
        // Queue full (at capacity)
        spdlog::warn("[BACKPRESSURE] Dispatcher MPSC queue full, dropping event");
        return false;
    }
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
        // Check pipeline state - respect Admin decisions
        if (pipeline_state_) {
            PipelineState state = pipeline_state_->getState();
            
            // If state is PAUSED or DRAINING, dispatcher should not consume events
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
        
        // Backpressure handling: Retry with exponential backoff if EventBus is full
        // This propagates backpressure from processors back to TCP ingest
        bool pushed = false;
        int retry_count = 0;
        constexpr int MAX_RETRIES = 3;
        
        while (!pushed && retry_count < MAX_RETRIES) {
            pushed = event_bus_.push(queueId, event_opt.value());
            
            if (!pushed) {
                retry_count++;
                if (retry_count < MAX_RETRIES) {
                    // Exponential backoff: 10us, 100us, 1ms
                    auto backoff_us = 10 * (1 << (retry_count - 1));
                    std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                    spdlog::debug("[BACKPRESSURE] EventBus queue {} full, retry {}/{} for event {}",
                                 static_cast<int>(queueId), retry_count, MAX_RETRIES, 
                                 event_opt.value()->header.id);
                }
            }
        }
        
        if (!pushed) {
            // After all retries failed, push to DLQ and log
            event_bus_.getDLQ().push(*event_opt.value());
            spdlog::warn("[BACKPRESSURE] Failed to push event {} to queue {} after {} retries. Pushed to DLQ.",
                        event_opt.value()->header.id, static_cast<int>(queueId), MAX_RETRIES);
        }
        
    }
    
    spdlog::info("Dispatcher DispatchLoop stopped.");
}

void Dispatcher::adaptToPressure(const EventPtr& evt) {
    if (!evt) return;
    
    auto rt_pressure = event_bus_.getRealtimePressure();
    
    if (rt_pressure == EventBusMulti::PressureLevel::CRITICAL) {
        // CRITICAL pressure: downgrade priorities by one level only
        // CRITICAL stays CRITICAL (never downgrade safety-critical events)
        // HIGH -> MEDIUM
        if (evt->header.priority == EventPriority::HIGH) {
            spdlog::debug("System CRITICAL pressure: Downgrading HIGH priority event {} to MEDIUM", evt->header.id);
            evt->header.priority = EventPriority::MEDIUM;
        }
        // Note: CRITICAL events are never downgraded - they are safety-critical
    } 
    else if (rt_pressure == EventBusMulti::PressureLevel::HIGH) {
        // HIGH pressure: only downgrade HIGH -> MEDIUM (conservative)
        if (evt->header.priority == EventPriority::HIGH) {
            spdlog::debug("System HIGH pressure: Downgrading HIGH priority event {} to MEDIUM", evt->header.id);
            evt->header.priority = EventPriority::MEDIUM;
        }
    }
}


