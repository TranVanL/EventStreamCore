#include "event/Dispatcher.hpp"
#include <algorithm>
#include <iostream>

using namespace EventStream;

void Dispatcher::start(){
    running_.store(true,std::memory_order_release);
    worker_thread_ = std::thread(&Dispatcher::DispatchLoop,this);
    spdlog::info("Dispatcher started.");
}

void Dispatcher::stop(){
    running_.store(false,std::memory_order_release);
    inbound_cv_.notify_all();
    if(worker_thread_.joinable()) {
        worker_thread_.join();
    }
    spdlog::info("Dispatcher stopped.");
}

bool Dispatcher::tryPush(const EventPtr& evt){
    std::unique_lock<std::mutex> lock(inbound_mutex_);
    if (inbound_queue_.size() >= inbound_capacity_)  return false;
    inbound_queue_.push_back(evt);
    inbound_cv_.notify_one();
    return true;
}

std::optional<EventPtr> Dispatcher::tryPop(std::chrono::milliseconds timeout){
    std::unique_lock <std::mutex> lock(inbound_mutex_);
    if(!inbound_cv_.wait_for(lock,timeout,[this]{ return !inbound_queue_.empty() || !running_.load(std::memory_order_acquire); })){
        return std::nullopt;
    }
    if(!running_.load(std::memory_order_acquire) && inbound_queue_.empty()){
        return std::nullopt;
    }
    EventPtr event = inbound_queue_.front();
    inbound_queue_.pop_front();
    return event;
}

EventBusMulti::QueueId Dispatcher::Route(const EventPtr& evt) {
    if (!evt) {
        spdlog::warn("Null event pointer in Route");
        return EventBusMulti::QueueId::TRANSACTIONAL;
    }

    EventBusMulti::QueueId queueId;
    EventPriority priority = EventPriority::MEDIUM;

    // If topic table exists and the topic is found, update priority from table
    if (topic_table_ && topic_table_->FoundTopic(evt->topic,priority) ) {
        spdlog::info("Found topic {} with priority {}", evt->topic, static_cast<int>(priority));
    }

    // Priority handling logic:
    // - If topic is found in table: only override if client priority is higher than table priority
    // - If topic is not found: maximum allowed priority is MEDIUM
    if (evt->header.priority < priority) {
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
        auto event = tryPop(std::chrono::milliseconds(100));
        if (!event.has_value()) continue;
        
        auto queueId = Route(event.value());
        bool pushed = event_bus_.push(queueId, event.value());

        if (!pushed) {
            spdlog::warn("Failed to push event {} to queue {} . Dropping event.", event.value()->header.id, static_cast<int>(queueId));
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
        if (evt->header.priority == EventPriority::HIGH) {
            evt->header.priority = EventPriority::MEDIUM;
        }
    }
}


