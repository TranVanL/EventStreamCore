#include <eventstream/core/processor/manager.hpp>
#include <eventstream/core/memory/numa.hpp>
#include <eventstream/core/metrics/registry.hpp>

ProcessManager::ProcessManager(EventStream::EventBusMulti& bus)
    : ProcessManager(bus, Dependencies{}) {}

ProcessManager::ProcessManager(EventStream::EventBusMulti& bus, const Dependencies& deps)
    : event_bus(bus),
      isRunning_(false),
      realtimeProcessor_(std::make_unique<RealtimeProcessor>(deps.alert_handler, deps.storage, deps.dlq)),
      transactionalProcessor_(std::make_unique<TransactionalProcessor>(deps.storage, deps.dlq)),
      batchProcessor_(std::make_unique<BatchProcessor>(deps.batch_window, &bus, deps.storage, deps.dlq)) {}

ProcessManager::~ProcessManager() noexcept {
    stop();
}

void ProcessManager::stop() {
    isRunning_.store(false, std::memory_order_release);
    if (realtimeThread_.joinable()) realtimeThread_.join();
    if (transactionalThread_.joinable()) transactionalThread_.join();
    if (batchThread_.joinable()) batchThread_.join();
    spdlog::info("ProcessManager stopped");
}

void ProcessManager::start() {
    isRunning_.store(true, std::memory_order_release);

    if (realtimeProcessor_) {
        realtimeThread_ = std::thread(&ProcessManager::runLoop, this,
            EventStream::EventBusMulti::QueueId::REALTIME, realtimeProcessor_.get());
        try {
            EventStream::NUMABinding::bindThread(realtimeThread_, 2);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to pin realtime thread: {}", e.what());
        }
    }

    if (transactionalProcessor_) {
        transactionalThread_ = std::thread(&ProcessManager::runLoop, this,
            EventStream::EventBusMulti::QueueId::TRANSACTIONAL, transactionalProcessor_.get());
    }

    if (batchProcessor_) {
        batchThread_ = std::thread(&ProcessManager::runLoop, this,
            EventStream::EventBusMulti::QueueId::BATCH, batchProcessor_.get());
    }

    spdlog::info("ProcessManager started");
}

void ProcessManager::runLoop(const EventStream::EventBusMulti::QueueId& qid, EventProcessor* processor) {
    processor->start();

    const auto timeout = (qid == EventStream::EventBusMulti::QueueId::REALTIME)
        ? std::chrono::milliseconds(10)
        : std::chrono::milliseconds(50);

    while (isRunning_.load(std::memory_order_acquire)) {
        auto opt = event_bus.pop(qid, timeout);
        if (!opt.has_value()) continue;

        try {
            processor->process(*opt.value());
        } catch (const std::exception& e) {
            spdlog::error("{} exception on event {}: {}",
                          processor->name(), opt.value()->header.id, e.what());
        }
    }

    processor->stop();
}

void ProcessManager::pauseTransactions() const {
    if (transactionalProcessor_) transactionalProcessor_->pauseProcessing();
}

void ProcessManager::resumeTransactions() const {
    if (transactionalProcessor_) transactionalProcessor_->resumeProcessing();
}

void ProcessManager::dropBatchEvents() const {
    if (batchProcessor_) batchProcessor_->dropBatchEvents();
}

void ProcessManager::resumeBatchEvents() const {
    if (batchProcessor_) batchProcessor_->resumeBatchEvents();
}

void ProcessManager::printLatencyMetrics() const {
    if (transactionalProcessor_)
        transactionalProcessor_->getLatencyHistogram().printPercentiles();
}
