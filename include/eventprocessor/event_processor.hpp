#pragma once
#include "event/EventBus.hpp"
#include <storage_engine/storage_engine.hpp>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include "utils/thread_pool.hpp"

class EventProcessor {
public:
    EventProcessor(EventStream::EventBus& bus, StorageEngine& storage, ThreadPool* pool = nullptr);
    ~EventProcessor();

    void init();
    void start();
    void stop();

private:
    // callback invoked by EventBus when an event is published; pushes to internal queue
    void onEvent(const EventStream::Event& event);
    // worker loop that processes queued events
    void processLoop();

    EventStream::EventBus& eventBus;
    StorageEngine& storageEngine;
    std::atomic<bool> isRunning;
    ThreadPool* workerPool = nullptr;
    std::thread processingThread;
    std::mutex incomingMutex;
    std::condition_variable incomingCv;
    std::queue<EventStream::Event> incomingQueue;
};