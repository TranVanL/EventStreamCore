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
    EventBusMulti::QueueID queueID;
    EventPriority priority = EventPriority::MEDIUM;

    // If topic table exists and the topic is found, update priority from table
    if (topic_table_ && topic_table_->FoundTopic(evt->topic,priority) ) {
        spdlog::info("Found topic {} with priority {}", evt->topic, static_cast<int>(priority));
    }

    // Priority handling logic:
    // - If topic is found in table: only override if client priority is higher than table priority
    // - If topic is not found: maximum allowed priority is MEDIUM
    if (evt->header.priority >= priority) {
        evt->header.priority = priority;
    }

    // Determine routes based on final priority
    if (evt->header.priority == EventPriority::CRITICAL || evt->header.priority == EventPriority::HIGH) {
        queueID = EventBusMulti::QueueId::REALTIME;
    }
    else if (evt->header.priority == EventPriority::MEDIUM) {
        queueID = EventBusMulti::QueueId::TRANSACTIONAL;
    }
    else { // LOW priority
        queueID = EventBusMulti::QueueId::BATCH;
    }

    return queueID;

}

void Dispatcher::DispatchLoop(){
    spdlog::info("Dispatcher DispatchLoop started.");
    while (running_.load(std::memory_order_acquire))
    {
        auto event = tryPop(std::chrono::milliseconds(100));
        if (!event.has_value()) continue;
        auto queueID = Route(event.value());
        event_bus_->push(queueID,event.value());

    }
    
    spdlog::info("Dispatcher DispatchLoop stopped.");
}
