// TransactionalProcessor Contract
// -------------------------------
// Input:
//   - Events from TRANSACTIONAL queue
// Guarantees:
//   - At-least-once processing
//   - Idempotent execution
//   - Retry on failure
//
// Output:
//   - StorageEngine (durable write)
//   - DLQ (failed after retries)
//
// Typical use cases:
//   - Database write
//   - Billing
//   - State mutation

#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/processor/processed_event_stream.hpp>

TransactionalProcessor::TransactionalProcessor(StorageEngine* storage,
                                               EventStream::DeadLetterQueue* dlq)
    : storage_(storage), dlq_(dlq) {}

TransactionalProcessor::~TransactionalProcessor() noexcept {
    spdlog::info("[DESTRUCTOR] TransactionalProcessor being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] TransactionalProcessor destroyed successfully");
}

void TransactionalProcessor::start() {
    spdlog::info("TransactionalProcessor started (max_retries: {}, storage: {}, dlq: {})",
                 max_retries_,
                 storage_ ? "enabled" : "disabled",
                 dlq_ ? "enabled" : "disabled");
    state_.store(ProcessState::RUNNING, std::memory_order_release);
}

void TransactionalProcessor::stop() {
    // Flush storage before stopping
    if (storage_) {
        storage_->flush();
    }
    state_.store(ProcessState::STOPPED, std::memory_order_release);
    spdlog::info("TransactionalProcessor stopped");
}

void TransactionalProcessor::process(const EventStream::Event& event) {
    // Bind processor thread to NUMA node on first call (lazy binding)
    static thread_local bool bound = false;
    if (!bound && numa_node_ >= 0) {
        EventStream::NUMABinding::bindThreadToNUMANode(numa_node_);
        bound = true;
    }

    auto &m = MetricRegistry::getInstance().getMetrics(name());

    // Check if paused by control plane
    if (paused_.load(std::memory_order_acquire)) {
        spdlog::debug("TransactionalProcessor paused, dropping event id {}", event.header.id);
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        
        // Push to DLQ when paused
        if (dlq_) {
            dlq_->push(event);
        }
        
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "processor_paused");
        return;
    }

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Lock-free idempotency check
    if (dedup_table_.is_duplicate(event.header.id, now_ms)) {
        spdlog::debug("Event id {} already processed (lock-free dedup)", event.header.id);
        return;  // Idempotent - already processed, don't count as drop
    }

    // Periodic cleanup (every 10 seconds, not in hot path)
    uint64_t last = last_cleanup_ms_.load(std::memory_order_acquire);
    if (last == 0 || now_ms - last > 10000) {
        if (last_cleanup_ms_.compare_exchange_strong(last, now_ms, 
                std::memory_order_release, std::memory_order_acquire)) {
            spdlog::debug("Performing idempotency table cleanup at {}", now_ms);
            dedup_table_.cleanup(now_ms);
        }
    }

    // Retry logic with exponential backoff
    bool success = false;
    for (int attempt = 1; attempt <= max_retries_; ++attempt) {
        if (handle(event)) {
            success = true;
            break;
        }
        if (attempt < max_retries_) {
            spdlog::warn("Transactional processing failed for event id {} (attempt {}/{}), retrying...", 
                        event.header.id, attempt, max_retries_);
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
        }
    }

    if (success) {
        // Record in dedup table ONLY after successful processing
        // This ensures failed events can be retried later
        if (!dedup_table_.insert(event.header.id, now_ms)) {
            spdlog::warn("Event id {} was processed concurrently, possible duplicate", event.header.id);
        }
        
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        
        // *** DURABLE WRITE TO STORAGE ***
        if (storage_) {
            storage_->storeEvent(event);
        }
        
        // Record latency
        if (event.dequeue_time_ns > 0) {
            uint64_t process_end_ns = EventStream::nowNs();
            uint64_t latency_ns = process_end_ns - event.dequeue_time_ns;
            latency_hist_.record(latency_ns);
        }
        
        MetricRegistry::getInstance().updateEventTimestamp(name());
        
        // Notify observers (for Distributed/Microservice hooks)
        EventStream::ProcessedEventStream::getInstance().notifyProcessed(event, name());
    } else {
        spdlog::error("Event id {} FAILED after {} retries - sending to Dead Letter Queue", 
                      event.header.id, max_retries_);
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        
        // *** PUSH TO DLQ ***
        if (dlq_) {
            dlq_->push(event);
        }
        
        // Notify observers
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "max_retries_exceeded");
    }
}

bool TransactionalProcessor::handle(const EventStream::Event& event) {
    // Business logic processing (simulated)
    // In real implementation, this would call external systems (DB, API, etc.)
    
    // Payment transaction handling
    if (event.topic.find("payment") != std::string::npos) {
        spdlog::debug("Processing payment transaction for event id {}", event.header.id);
        return true;
    }
    
    // Audit log handling
    if (event.topic.find("audit") != std::string::npos) {
        spdlog::debug("Recording audit log for event id {}", event.header.id);
        return true;
    }
    
    // State mutation handling
    if (event.topic.find("state") != std::string::npos) {
        spdlog::debug("Processing state change for event id {}", event.header.id);
        return true;
    }
    
    // Default transactional handling
    spdlog::debug("Transactional processing event id {} from topic {}", 
                  event.header.id, event.topic);
    return true;
}