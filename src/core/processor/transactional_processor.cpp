/// TransactionalProcessor — at-least-once, idempotent event processing
/// with configurable retry, lock-free deduplication, and DLQ fallback.
/// Flow: dedup → validate → rules → enrich → handle(retry) → decide → notify

#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/processor/processed_event_stream.hpp>
#include <eventstream/core/processor/event_handler.hpp>

static const EventStream::DefaultEventHandler s_default_handler;

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
}

void TransactionalProcessor::stop() {
    if (storage_) {
        storage_->flush();
    }
    spdlog::info("TransactionalProcessor stopped");
}

void TransactionalProcessor::process(const EventStream::Event& event) {
    // NUMA binding (lazy)
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
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "processor_paused");
        return;
    }

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // ── Step 1: Lock-free idempotency check (dedup) ──
    if (dedup_table_.is_duplicate(event.header.id, now_ms)) {
        spdlog::debug("Event id {} already processed (dedup)", event.header.id);
        return;
    }

    // Periodic cleanup
    uint64_t last = last_cleanup_ms_.load(std::memory_order_acquire);
    if (last == 0 || now_ms - last > 10000) {
        if (last_cleanup_ms_.compare_exchange_strong(last, now_ms, 
                std::memory_order_release, std::memory_order_acquire)) {
            dedup_table_.cleanup(now_ms);
        }
    }

    // ── Step 2: Lookup handler ──
    auto* handler = EventStream::EventHandlerRegistry::getInstance().getHandler(event.topic);
    if (!handler) handler = const_cast<EventStream::DefaultEventHandler*>(&s_default_handler);

    // ── Step 3: Validate ──
    auto validation = handler->validate(event);
    if (!validation.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[Txn] Validation failed event_id={}: {}", event.header.id, validation.error);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), validation.error.c_str());
        return;
    }

    // ── Step 4: Rule checking ──
    auto rules = handler->checkRules(event);
    if (!rules.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[Txn] Rule check failed event_id={}: {}", event.header.id, rules.error);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), rules.error.c_str());
        return;
    }

    // ── Step 5: Core processing with retry + exponential backoff ──
    bool success = false;
    for (int attempt = 1; attempt <= max_retries_; ++attempt) {
        auto result = handler->handle(event);
        
        if (result.outcome == EventStream::HandleOutcome::SUCCESS ||
            result.outcome == EventStream::HandleOutcome::ALERT) {
            success = true;
            break;
        }
        
        if (attempt < max_retries_) {
            spdlog::warn("[Txn] Failed event_id={} (attempt {}/{}): {}, retrying...", 
                        event.header.id, attempt, max_retries_, result.message);
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
        }
    }

    // ── Step 6: Decide outcome ──
    if (success) {
        // Record in dedup table after success
        if (!dedup_table_.insert(event.header.id, now_ms)) {
            spdlog::warn("Event id {} processed concurrently, possible duplicate", event.header.id);
        }
        
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        
        // Durable write
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
        
        // Notify observers
        EventStream::ProcessedEventStream::getInstance().notifyProcessed(event, name());
    } else {
        spdlog::error("Event id {} FAILED after {} retries -> DLQ", 
                      event.header.id, max_retries_);
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "max_retries_exceeded");
    }
}

bool TransactionalProcessor::handle(const EventStream::Event& event) {
    // Legacy fallback
    auto* handler = EventStream::EventHandlerRegistry::getInstance().getHandler(event.topic);
    if (handler) {
        auto result = handler->handle(event);
        return result.outcome != EventStream::HandleOutcome::FAIL;
    }
    return true;
}
