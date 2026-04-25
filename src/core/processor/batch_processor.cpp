/// BatchProcessor — windowed aggregation with per-topic bucketing.
/// Flow per event: validate → rules → enrich → bucket
/// Flow on flush: handle each event → storage → notify observers

#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/processor/processed_event_stream.hpp>
#include <eventstream/core/processor/event_handler.hpp>
#include <spdlog/spdlog.h>

using Clock = std::chrono::steady_clock;

static const EventStream::DefaultEventHandler s_default_handler;

BatchProcessor::BatchProcessor(std::chrono::seconds window, 
                               EventStream::EventBusMulti* bus,
                               StorageEngine* storage,
                               EventStream::DeadLetterQueue* dlq)
    : window_(window), event_bus_(bus), storage_(storage), dlq_(dlq) {}

BatchProcessor::~BatchProcessor() noexcept {
    spdlog::info("[DESTRUCTOR] BatchProcessor being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] BatchProcessor destroyed successfully");
}

void BatchProcessor::start() {
    spdlog::info("BatchProcessor started (window: {}s, storage: {}, dlq: {})", 
                 window_.count(),
                 storage_ ? "enabled" : "disabled",
                 dlq_ ? "enabled" : "disabled");
}

void BatchProcessor::stop() {
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    for (auto& [topic, bucket] : buckets_) {
        std::lock_guard<std::mutex> lock(bucket.bucket_mutex);
        flushBucketLocked(bucket, topic);
    }
    if (storage_) {
        storage_->flush();
    }
    spdlog::info("BatchProcessor stopped");
}

void BatchProcessor::flush(const std::string& topic) {
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    auto it = buckets_.find(topic);
    if (it == buckets_.end()) return;
    
    TopicBucket& bucket = it->second;
    std::lock_guard<std::mutex> lock(bucket.bucket_mutex);
    flushBucketLocked(bucket, topic);
}

void BatchProcessor::process(const EventStream::Event& event) {
    // NUMA binding (lazy)
    static thread_local bool bound = false;
    if (!bound && numa_node_ >= 0) {
        EventStream::NUMABinding::bindThreadToNUMANode(numa_node_);
        bound = true;
    }

    auto &m = MetricRegistry::getInstance().getMetrics(name());

    // Control plane drop
    if (drop_events_.load(std::memory_order_acquire)) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        if (event_bus_) {
            size_t dropped = event_bus_->dropBatchFromQueue(EventStream::EventBusMulti::QueueId::BATCH);
            if (dropped > 0) {
                spdlog::warn("[Batch] Batch drop: dropped {} events to DLQ", dropped);
            }
        }
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "control_plane_drop");
        return;
    }

    // ── Step 1: Lookup handler ──
    auto* handler = EventStream::EventHandlerRegistry::getInstance().getHandler(event.topic);
    if (!handler) handler = const_cast<EventStream::DefaultEventHandler*>(&s_default_handler);

    // ── Step 2: Validate ──
    auto validation = handler->validate(event);
    if (!validation.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[Batch] Validation failed event_id={}: {}", event.header.id, validation.error);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), validation.error.c_str());
        return;
    }

    // ── Step 3: Rule checking ──
    auto rules = handler->checkRules(event);
    if (!rules.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[Batch] Rule check failed event_id={}: {}", event.header.id, rules.error);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), rules.error.c_str());
        return;
    }

    // ── Step 4: Bucket the event (aggregate) ──
    auto now = Clock::now();
    
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    auto [it, inserted] = buckets_.try_emplace(event.topic);
    TopicBucket& bucket = it->second;
    
    {
        std::lock_guard<std::mutex> lock(bucket.bucket_mutex);

        bucket.events.push_back(event);
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);

        if (bucket.last_flush_time.time_since_epoch().count() == 0) {
            bucket.last_flush_time = now;
            MetricRegistry::getInstance().updateEventTimestamp(name());
            return;
        }

        // Flush when window expires
        if (now - bucket.last_flush_time >= window_) {
            flushBucketLocked(bucket, event.topic);
            bucket.last_flush_time = now;
        }

        MetricRegistry::getInstance().updateEventTimestamp(name());
    }
}

void BatchProcessor::flushBucketLocked(TopicBucket& bucket, const std::string& topic) {
    if (bucket.events.empty()) return;
    size_t count = bucket.events.size();
    
    spdlog::info("[BATCH FLUSH] topic={} count={} (window={}s)", 
                 topic, count, window_.count());
    
    // Aggregate metrics
    uint64_t total_bytes = 0;
    uint32_t min_id = UINT32_MAX, max_id = 0;
    
    for (const auto& evt : bucket.events) {
        total_bytes += evt.body.size();
        min_id = std::min(min_id, evt.header.id);
        max_id = std::max(max_id, evt.header.id);
    }
    
    double avg_bytes = total_bytes / static_cast<double>(count);
    spdlog::debug("  [BATCH] Aggregated: {} events, {} bytes, avg {:.1f}b, id_range [{}, {}]",
                  count, total_bytes, avg_bytes, min_id, max_id);

    // ── Handle each event via handler ──
    auto* handler = EventStream::EventHandlerRegistry::getInstance().getHandler(topic);
    if (!handler) handler = const_cast<EventStream::DefaultEventHandler*>(&s_default_handler);

    for (const auto& evt : bucket.events) {
        auto result = handler->handle(evt);
        
        if (result.outcome == EventStream::HandleOutcome::FAIL ||
            result.outcome == EventStream::HandleOutcome::DROP) {
            spdlog::warn("[BATCH] Event id {} failed in batch: {}", evt.header.id, result.message);
            if (dlq_) dlq_->push(evt);
            EventStream::ProcessedEventStream::getInstance().notifyDropped(
                evt, "BatchProcessor", result.message.c_str());
            continue;
        }
        
        // Persist
        if (storage_) {
            storage_->storeEvent(evt);
        }
        
        // Notify observers
        EventStream::ProcessedEventStream::getInstance().notifyProcessed(evt, "BatchProcessor");
    }
    
    if (storage_) {
        storage_->flush();
        spdlog::debug("  [BATCH] Persisted {} events to storage", count);
    }
    
    bucket.events.clear();
}
