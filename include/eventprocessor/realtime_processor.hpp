#pragma once
#include "eventprocessor/event_processor.hpp"
#include <storage_engine/storage_engine.hpp>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include "utils/thread_pool.hpp"

class RealtimeProcessor : public EventProcessor {
public:
    RealtimeProcessor(EventStream::EventBusMulti& bus, StorageEngine& storage, ThreadPool* pool = nullptr);

    virtual ~RealtimeProcessor();

    virtual void start() override;

    virtual void stop() override;

    virtual void processLoop() override;

};