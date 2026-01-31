#include <eventstream/core/processor/process_manager.hpp>
#include <eventstream/core/memory/numa_binding.hpp>
#include <eventstream/core/metrics/registry.hpp>

// Backward compatible constructor (no dependencies)
ProcessManager::ProcessManager(EventStream::EventBusMulti& bus)
    : ProcessManager(bus, Dependencies{}) {}

// Full constructor with dependencies
ProcessManager::ProcessManager(EventStream::EventBusMulti& bus, const Dependencies& deps)
    : event_bus(bus),
      isRunning_(false),
      realtimeProcessor_(std::make_unique<RealtimeProcessor>(deps.alert_handler, deps.storage, deps.dlq)),
      transactionalProcessor_(std::make_unique<TransactionalProcessor>(deps.storage, deps.dlq)),
      batchProcessor_(std::make_unique<BatchProcessor>(deps.batch_window, &bus, deps.storage, deps.dlq)) {
    
    spdlog::info("[ProcessManager] Initialized with dependencies:");
    spdlog::info("  - Storage: {}", deps.storage ? "enabled" : "disabled");
    spdlog::info("  - DLQ: {}", deps.dlq ? "enabled" : "disabled");
    spdlog::info("  - AlertHandler: {}", deps.alert_handler ? deps.alert_handler->name() : "default");
    spdlog::info("  - BatchWindow: {}s", deps.batch_window.count());
}

ProcessManager::~ProcessManager() noexcept {
    spdlog::info("[DESTRUCTOR] ProcessManager being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] ProcessManager destroyed successfully");
}

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
        // Pin realtime thread to core 2 for low-latency processing
        try {
            EventStream::NUMABinding::bindThread(realtimeThread_, 2);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to pin RealtimeProcessor thread to core 2: {}", e.what());
        }
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
    processor->start();
    
    // Cache metrics registry reference to avoid repeated getInstance() calls
    auto& metrics_registry = MetricRegistry::getInstance();
    
    // Determine optimal timeout based on queue type
    const auto timeout_ms = (qid == EventStream::EventBusMulti::QueueId::REALTIME) 
        ? std::chrono::milliseconds(10)   // Low latency for realtime
        : std::chrono::milliseconds(50);  // Higher tolerance for batch/transactional
    
    while(isRunning_.load(std::memory_order_acquire)) {
        auto eventOpt = event_bus.pop(qid, timeout_ms);
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
// ============================================================================
// Control Plane Actions
// ============================================================================

void ProcessManager::pauseTransactions() const {
    if (transactionalProcessor_) {
        transactionalProcessor_->pauseProcessing();
        spdlog::warn("CONTROL ACTION: Pausing TransactionalProcessor");
    }
}

void ProcessManager::resumeTransactions() const {
    if (transactionalProcessor_) {
        transactionalProcessor_->resumeProcessing();
        spdlog::info("CONTROL ACTION: Resuming TransactionalProcessor");
    }
}

void ProcessManager::dropBatchEvents() const {
    if (batchProcessor_) {
        batchProcessor_->dropBatchEvents();
        spdlog::warn("CONTROL ACTION: Dropping BatchProcessor events");
    }
}

void ProcessManager::resumeBatchEvents() const {
    if (batchProcessor_) {
        batchProcessor_->resumeBatchEvents();
        spdlog::info("CONTROL ACTION: Resuming BatchProcessor events");
    }
}

void ProcessManager::printLatencyMetrics() const {
    if (transactionalProcessor_) {
        spdlog::info(" ");
        transactionalProcessor_->getLatencyHistogram().printPercentiles();
    }
}