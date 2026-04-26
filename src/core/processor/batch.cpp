#include <eventstream/core/processor/processor.hpp>
#include <eventstream/core/processor/output.hpp>
#include <eventstream/core/processor/handler.hpp>
#include <spdlog/spdlog.h>

using Clock = std::chrono::steady_clock;

BatchProcessor::BatchProcessor(std::chrono::seconds window,
                               EventStream::EventBusMulti* bus,
                               StorageEngine* storage,
                               EventStream::DeadLetterQueue* dlq)
    : window_(window), event_bus_(bus), storage_(storage), dlq_(dlq) {}

BatchProcessor::~BatchProcessor() noexcept {
    stop();
}

void BatchProcessor::start() {
    spdlog::info("BatchProcessor started (window={}s, storage={}, dlq={})",
                 window_.count(), storage_ ? "on" : "off", dlq_ ? "on" : "off");
}

void BatchProcessor::stop() {
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    for (auto& [topic, bucket] : buckets_) {
        std::lock_guard<std::mutex> lock(bucket.bucket_mutex);
        flushBucketLocked(bucket, topic);
    }
    if (storage_) storage_->flush();
    spdlog::info("BatchProcessor stopped");
}

void BatchProcessor::flush(const std::string& topic) {
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    auto it = buckets_.find(topic);
    if (it == buckets_.end()) return;
    std::lock_guard<std::mutex> lock(it->second.bucket_mutex);
    flushBucketLocked(it->second, topic);
}

void BatchProcessor::process(const EventStream::Event& event) {
    static thread_local bool numa_bound = false;
    if (!numa_bound && numa_node_ >= 0) {
        EventStream::NUMABinding::bindThreadToNUMANode(numa_node_);
        numa_bound = true;
    }

    auto& m = MetricRegistry::getInstance().getMetrics(name());

    // Control plane drop
    if (drop_events_.load(std::memory_order_acquire)) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        if (event_bus_) {
            event_bus_->dropBatchFromQueue(EventStream::EventBusMulti::QueueId::BATCH);
        }
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), "backpressure_drop");
        return;
    }

    auto* handler = EventStream::getOrDefault(event.topic);

    auto v = handler->validate(event);
    if (!v.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), v.error.c_str());
        return;
    }

    auto r = handler->checkRules(event);
    if (!r.valid) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        if (dlq_) dlq_->push(event);
        EventStream::ProcessedEventStream::getInstance().notifyDropped(event, name(), r.error.c_str());
        return;
    }

    // Bucket the event
    auto now = Clock::now();
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    auto [it, _] = buckets_.try_emplace(event.topic);
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
    spdlog::info("[BATCH FLUSH] topic={} count={}", topic, count);

    auto* handler = EventStream::getOrDefault(topic);

    for (const auto& evt : bucket.events) {
        auto result = handler->handle(evt);
        if (result.outcome == EventStream::HandleOutcome::FAIL ||
            result.outcome == EventStream::HandleOutcome::DROP) {
            if (dlq_) dlq_->push(evt);
            EventStream::ProcessedEventStream::getInstance().notifyDropped(
                evt, name(), result.message.c_str());
            continue;
        }
        if (storage_) storage_->storeEvent(evt);
        EventStream::ProcessedEventStream::getInstance().notifyProcessed(evt, name());
    }

    if (storage_) storage_->flush();
    bucket.events.clear();
}
