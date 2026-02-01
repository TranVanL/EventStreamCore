#include <eventstream/core/processor/event_processor.hpp>
#include <eventstream/core/processor/processed_event_stream.hpp>
#include <spdlog/spdlog.h>

using namespace EventStream;
using Clock = std::chrono::steady_clock;

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
    // Flush all remaining events with proper synchronization
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    for (auto& [topic, bucket] : buckets_) {
        std::lock_guard<std::mutex> lock(bucket.bucket_mutex);
        flushBucketLocked(bucket, topic);
    }
    
    // Flush storage
    if (storage_) {
        storage_->flush();
    }
    spdlog::info("BatchProcessor stopped");
}

void BatchProcessor::flush(const std::string& topic) {
    // Thread-safe flush for external callers
    // Acquires both locks properly to avoid race conditions
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    auto it = buckets_.find(topic);
    if (it == buckets_.end())
        return;
    
    TopicBucket& bucket = it->second;
    std::lock_guard<std::mutex> lock(bucket.bucket_mutex);
    flushBucketLocked(bucket, topic);
}

void BatchProcessor::process(const EventStream::Event& event) {
    // Bind processor thread to NUMA node on first call (lazy binding)
    static thread_local bool bound = false;
    if (!bound && numa_node_ >= 0) {
        EventStream::NUMABinding::bindThreadToNUMANode(numa_node_);
        bound = true;
    }

    auto &m = MetricRegistry::getInstance().getMetrics(name());

    // Check if dropping events by control plane
    if (drop_events_.load(std::memory_order_acquire)) {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        
        // Push to DLQ
        if (dlq_) {
            dlq_->push(event);
        }
        
        // Batch drop from queue if available
        if (event_bus_) {
            size_t dropped = event_bus_->dropBatchFromQueue(EventStream::EventBusMulti::QueueId::BATCH);
            if (dropped > 0) {
                spdlog::warn("[BatchProcessor] Batch drop triggered: dropped {} events to DLQ", dropped);
            }
        }
        
        EventStream::ProcessedEventStream::getInstance().notifyDropped(
            event, name(), "control_plane_drop");
        return;
    }

    auto now = Clock::now();

    // Safe map access with proper synchronization
    // CRITICAL FIX: Keep map_lock held during entire bucket operation to prevent
    // iterator invalidation from rehashing. Do NOT release map_lock early.
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    auto [it, inserted] = buckets_.try_emplace(event.topic);
    TopicBucket& bucket = it->second;
    
    // Acquire bucket-level lock for the specific topic
    // FIX: Keep map_lock held to ensure bucket reference remains valid
    {
        std::lock_guard<std::mutex> lock(bucket.bucket_mutex);
        
        // FIX: Do NOT unlock map_lock here - bucket reference could become invalid
        // The performance impact is minimal since bucket operations are fast

        bucket.events.push_back(event);
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);

        // Initialize flush time on first event
        if (bucket.last_flush_time.time_since_epoch().count() == 0) {
            bucket.last_flush_time = now;
            MetricRegistry::getInstance().updateEventTimestamp(name());
            return;
        }

        // Check if window expired - flush while holding bucket lock
        if (now - bucket.last_flush_time >= window_) {
            flushBucketLocked(bucket, event.topic);
            bucket.last_flush_time = now;
        }

        MetricRegistry::getInstance().updateEventTimestamp(name());
    }
}

void BatchProcessor::flushBucketLocked(TopicBucket& bucket, const std::string& topic) {
    // NOTE: Must be called while holding bucket.bucket_mutex
    // This variant takes bucket reference directly to avoid map lookup race
    
    if (bucket.events.empty())
        return;
    size_t count = bucket.events.size();
    
    spdlog::info("[BATCH FLUSH] topic={} count={} (window={}s)", 
                 topic, count, window_.count());
    
    // Aggregate batch metrics
    uint64_t total_bytes = 0;
    uint32_t min_id = UINT32_MAX;
    uint32_t max_id = 0;
    
    for (const auto& evt : bucket.events) {
        total_bytes += evt.body.size();
        min_id = std::min(min_id, evt.header.id);
        max_id = std::max(max_id, evt.header.id);
    }
    
    double avg_bytes = total_bytes / static_cast<double>(count);
    spdlog::debug("  [BATCH] Aggregated: {} events, {} bytes, avg {:.1f}b, id_range [{}, {}]",
                  count, total_bytes, avg_bytes, min_id, max_id);
    
    // *** BATCH WRITE TO STORAGE ***
    if (storage_) {
        for (const auto& evt : bucket.events) {
            storage_->storeEvent(evt);
        }
        storage_->flush();
        spdlog::debug("  [BATCH] Persisted {} events to storage", count);
    }
    
    // Notify observers for each event in batch
    for (const auto& evt : bucket.events) {
        EventStream::ProcessedEventStream::getInstance().notifyProcessed(evt, name());
    }
    
    bucket.events.clear();
}
