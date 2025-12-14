#include "eventprocessor/processManager.hpp"

void ProcessManager::stop() {
    isRunning_.store(false, std::memory_order_release);
    if (realtimeThread_.joinable()) {
        realtimeThread_.join();
    }
    if (transactionalThread_.joinable()) {
        transactionalThread_.join();
    }
    if (batchThread_.joinable()) {
        batchThread_.join();
    }
    spdlog::info("ProcessManager stopped.");
}

void ProcessManager::start(){
    isRunning_.store(true, std::memory_order_release);
    spdlog::info("ProcessManager started.");

    if (realtimeProcessor_) {
        realtimeThread_ = std::thread(&ProcessManager::runLoop,this,EventStream::EventBusMulti::QueueId::REALTIME,realtimeProcessor_.get());
    }

    if (transactionalProcessor_) {
        transactionalThread_ = std::thread(&ProcessManager::runLoop,this,EventStream::EventBusMulti::QueueId::TRANSACTIONAL,transactionalProcessor_.get());
    }

    if (batchProcessor_) {
        batchThread_ = std::thread(&ProcessManager::runLoop,this,EventStream::EventBusMulti::QueueId::BATCH,batchProcessor_.get());
    }
}

void ProcessManager::runLoop(const EventStream::EventBusMulti::QueueId& qid, EventProcessor* processor) {
    spdlog::info("Processor {} started.", processor->name());
    processor ->start();
    while(isRunning_.load(std::memory_order_acquire)) {
        auto eventOpt = event_bus.pop(qid, std::chrono::milliseconds(100));
        if (!eventOpt.has_value()) continue;
        auto& event = eventOpt.value();
        try {
            processor->process(*event);
        } catch (const std::exception& e) {
            spdlog::error("Processor {} failed to process event id {}: {}", processor->name(), event->header.id, e.what());
        }
        
    }
    processor->stop();
    spdlog::info("Processor {} stopped.", processor->name());
}