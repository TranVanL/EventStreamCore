#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/processor/processed_event_stream.hpp>
#include <eventstream/core/processor/event_handler.hpp>

TransactionalProcessor::TransactionalProcessor(StorageEngine* storage,
                                               EventStream::DeadLetterQueue* dlq)
    : storage_(storage), dlq_(dlq) {}

TransactionalProcessor::~TransactionalProcessor() noexcept {
    stop();
}

void TransactionalProcessor::start() {
    spdlog::info("TransactionalProcessor started (retries={}, storage={}, dlq={})",
                 max_retries_, storage_ ? "on" : "off", dlq_ ? "on" : "off");
}

void TransactionalProcessor::stop() {
    if (storage_) storage_->flush();
    spdlog::info("TransactionalProcessor stopped");
}

void TransactionalProcessor::process(const EventStream::Event& event) {
    static thread_local bool numa_bound = false;
    if (!numa_bound && numa_node_ >= 0) {
        EventStream::NUMABinding::bindThreadToNUMANode(numa_node_);
        numa_bound = true;
    }

    auto& m = MetricRegistry::getInstance().getMetrics(name());

    if (paused_.load(std::memory_order_acquire)) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), "processor_paused");
        return;
    }

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Dedup
    if (dedup_table_.is_duplicate(event.header.id, now_ms))
        return;

    uint64_t last = last_cleanup_ms_.load(std::memory_order_acquire);
    if (last == 0 || now_ms - last > 10000) {
        if (last_cleanup_ms_.compare_exchange_strong(last, now_ms,
                std::memory_order_release, std::memory_order_acquire))
            dedup_table_.cleanup(now_ms);
    }

    auto* handler = EventStream::getOrDefault(event.topic);

    // Validate
    auto v = handler->validate(event);
    if (!v.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), v.error.c_str());
        return;
    }

    // Rules
    auto r = handler->checkRules(event);
    if (!r.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), r.error.c_str());
        return;
    }

    // Handle with retry
    bool success = false;
    for (int attempt = 1; attempt <= max_retries_; ++attempt) {
        auto result = handler->handle(event);
        if (result.outcome == EventStream::HandleOutcome::SUCCESS ||
            result.outcome == EventStream::HandleOutcome::ALERT) {
            success = true;
            break;
        }
        if (attempt < max_retries_) {
            spdlog::warn("[Txn] event_id={} attempt {}/{} failed, retrying",
                         event.header.id, attempt, max_retries_);
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
        }
    }

    if (success) {
        dedup_table_.insert(event.header.id, now_ms);
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        if (storage_) storage_->storeEvent(event);

        if (event.dequeue_time_ns > 0) {
            uint64_t latency = EventStream::nowNs() - event.dequeue_time_ns;
            latency_hist_.record(latency);
        }

        MetricRegistry::getInstance().updateEventTimestamp(name());
        EventStream::ProcessedEventStream::getInstance().notifyProcessed(event, name());
    } else {
        spdlog::error("event_id={} failed after {} retries -> DLQ", event.header.id, max_retries_);
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), "max_retries_exceeded");
    }
}

bool TransactionalProcessor::handle(const EventStream::Event& event) {
    auto result = EventStream::getOrDefault(event.topic)->handle(event);
    return result.outcome != EventStream::HandleOutcome::FAIL;
}
