#include "eventprocessor/event_processor.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

EventProcessor::EventProcessor(EventStream::EventBus& bus, StorageEngine& storage, ThreadPool* pool)
    : eventBus(bus), storageEngine(storage), isRunning(false), workerPool(pool) {
}

EventProcessor::~EventProcessor() {
    stop();
}

void EventProcessor::init() {
    spdlog::info("EventProcessor initialized.");
    // subscribe once; handler pushes events into internal queue for async processing
    eventBus.subscribe([this](const EventStream::Event& event){
        spdlog::info("EventProcessor received event ID {} for processing", event.header.id);
        this->onEvent(event);
    });
}

void EventProcessor::start() {
    isRunning.store(true, std::memory_order_release);
    processingThread = std::thread(&EventProcessor::processLoop, this);
    spdlog::info("EventProcessor started.");
}

void EventProcessor::stop() {
    isRunning.store(false, std::memory_order_release);
    // Wake processing thread
    incomingCv.notify_all();
    if (processingThread.joinable()) processingThread.join();
    spdlog::info("EventProcessor stopped.");
}

void EventProcessor::onEvent(const EventStream::Event& event) {
    // called from publisher thread; push event into internal queue quickly
    {
        std::lock_guard<std::mutex> lk(incomingMutex);
        incomingQueue.push(event);
    }
    incomingCv.notify_one();
}

void EventProcessor::processLoop() {
    while (isRunning.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(incomingMutex);
        incomingCv.wait(lk, [this]{ return !incomingQueue.empty() || !isRunning.load(); });

        while (!incomingQueue.empty()) {
            EventStream::Event ev = std::move(incomingQueue.front());
            incomingQueue.pop();
            lk.unlock();

            try {
                if (workerPool) {
                    // offload storage to thread pool
                    workerPool->submit([ev, this](){
                        try {
                            storageEngine.storeEvent(ev);
                            spdlog::info("Stored event ID {} from source type {}", ev.header.id, static_cast<int>(ev.header.sourceType));
                        } catch (const std::exception& e) {
                            spdlog::error("Failed to store event ID {}: {}", ev.header.id, e.what());
                        }
                    });
                } else {
                    storageEngine.storeEvent(ev);
                    spdlog::info("Stored event ID {} from source type {}", ev.header.id, static_cast<int>(ev.header.sourceType));
                }
            } catch (const std::exception& e) {
                spdlog::error("Failed to schedule/store event ID {}: {}", ev.header.id, e.what());
            }

            lk.lock();
        }
    }
    // drain any remaining events after isRunning == false
    while (true) {
        std::lock_guard<std::mutex> lk(incomingMutex);
        if (incomingQueue.empty()) break;
        EventStream::Event ev = std::move(incomingQueue.front());
        incomingQueue.pop();
        try {
            if (workerPool) {
                workerPool->submit([ev, this]() {
                    try { storageEngine.storeEvent(ev); } catch (...) {}
                });
            } else {
                storageEngine.storeEvent(ev);
            }
        } catch (...) {}
    }
}