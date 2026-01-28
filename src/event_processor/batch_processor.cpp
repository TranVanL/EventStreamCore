#include "eventprocessor/EventProcessor.hpp"
#include <spdlog/spdlog.h>

using namespace EventStream;
using Clock = std::chrono::steady_clock;

BatchProcessor::BatchProcessor(std::chrono::seconds window, EventStream::EventBusMulti* bus)
    : window_(window), event_bus_(bus) {}

BatchProcessor::~BatchProcessor() noexcept {
    spdlog::info("[DESTRUCTOR] BatchProcessor being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] BatchProcessor destroyed successfully");
}

void BatchProcessor::start() {
    spdlog::info("BatchProcessor started, window={}s", window_.count());
}

void BatchProcessor::stop() {
    // Flush all remaining events with proper synchronization
    std::lock_guard<std::mutex> map_lock(buckets_mutex_);
    for (auto& [topic, bucket] : buckets_) {
        std::lock_guard<std::mutex> lock(bucket.bucket_mutex);
        flush(topic);
    }
    spdlog::info("BatchProcessor stopped");
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
        // Batch drop from queue to DLQ
        if (event_bus_) {
            size_t dropped = event_bus_->dropBatchFromQueue(EventStream::EventBusMulti::QueueId::BATCH);
            if (dropped > 0) {
                spdlog::warn("[BatchProcessor] Batch drop triggered: dropped {} events to DLQ", dropped);
            }
        } else {
            // Fallback: single event drop
            m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
            spdlog::debug("BatchProcessor dropping event id {}", event.header.id);
        }
        return;
    }

    auto now = Clock::now();

    // CRITICAL FIX: Safe map access with proper synchronization
    // First acquire map-level lock to prevent reallocation
    std::unique_lock<std::mutex> map_lock(buckets_mutex_);
    auto [it, inserted] = buckets_.try_emplace(event.topic);
    TopicBucket& bucket = it->second;
    map_lock.unlock();
    
    // Now acquire bucket-level lock for the specific topic
    // Day 39 Optimization: Single map lookup eliminates dual O(1) -> O(1) access
    {
        std::lock_guard<std::mutex> lock(bucket.bucket_mutex);

        bucket.events.push_back(event);
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);

        // OPTIMIZATION: last_flush_time now embedded in TopicBucket (no second map lookup)
        if (bucket.last_flush_time.time_since_epoch().count() == 0) {
            bucket.last_flush_time = now;
            MetricRegistry::getInstance().updateEventTimestamp(name());
            return;
        }

        if (now - bucket.last_flush_time >= window_) {
            flush(event.topic);
            bucket.last_flush_time = now;
        }

        MetricRegistry::getInstance().updateEventTimestamp(name());
    }
}

void BatchProcessor::flush(const std::string& topic) {
    auto it = buckets_.find(topic);
    if (it == buckets_.end() || it->second.events.empty())
        return;
    
    size_t count = it->second.events.size();
    spdlog::info("[BATCH FLUSH] topic={} count={} (window={}s)", 
                 topic, count, window_.count());
    
    // Aggregate batch metrics
    uint64_t total_bytes = 0;
    uint32_t min_id = UINT32_MAX;
    uint32_t max_id = 0;
    
    for (const auto& evt : it->second.events) {
        total_bytes += evt.body.size();
        min_id = std::min(min_id, evt.header.id);
        max_id = std::max(max_id, evt.header.id);
    }
    
    double avg_bytes = total_bytes / static_cast<double>(count);
    spdlog::debug("  [BATCH] Aggregated: {} events, {} bytes, avg {:.1f}b, id_range [{}, {}]",
                  count, total_bytes, avg_bytes, min_id, max_id);
    
    // Note: Storage persistence should be handled by the event processor or a dedicated 
    // storage sink. Consider integrating with StorageEngine for batch persistence in future.
    
    it->second.events.clear();
}
