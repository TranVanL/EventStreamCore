#include "eventprocessor/event_processor.hpp"
#include <spdlog/spdlog.h>

using namespace EventStream;
using Clock = std::chrono::steady_clock;

BatchProcessor::BatchProcessor(std::chrono::seconds window)
    : window_(window) {
}

BatchProcessor::~BatchProcessor() noexcept {
    spdlog::info("[DESTRUCTOR] BatchProcessor being destroyed...");
    stop();
    spdlog::info("[DESTRUCTOR] BatchProcessor destroyed successfully");
}

void BatchProcessor::start() {
    spdlog::info("BatchProcessor started, window={}s", window_.count());
}

void BatchProcessor::stop() {
    // Flush all remaining events
    std::lock_guard<std::mutex> lock(buckets_mutex_);
    for (auto& [topic, _] : buckets_) {
        flush(topic);
    }
    spdlog::info("BatchProcessor stopped");
}

void BatchProcessor::process(const EventStream::Event& event) {
    auto &m = MetricRegistry::getInstance().getMetrics(name());
    auto now = Clock::now();
    
    {
        std::lock_guard<std::mutex> lock(buckets_mutex_);
        
        // Add event to bucket for this topic
        auto& bucket = buckets_[event.topic];
        bucket.push_back(event);
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        
        spdlog::debug("BatchProcessor accumulated event id: {} topic: {} (bucket size: {})", 
                     event.header.id, event.topic, bucket.size());
        
        // Check if we need to flush based on time window
        auto& last = last_flush_[event.topic];
        if (last.time_since_epoch().count() == 0) {
            // First event for this topic
            last = now;
            // Update last event timestamp (for stale detection)
            MetricRegistry::getInstance().updateEventTimestamp(name());
            return;
        }
        
        if (now - last >= window_) {
            flush(event.topic);
            last = now;
        }
        
        // Update last event timestamp (for stale detection)
        MetricRegistry::getInstance().updateEventTimestamp(name());
    }
}

void BatchProcessor::flush(const std::string& topic) {
    auto it = buckets_.find(topic);
    if (it == buckets_.end() || it->second.empty())
        return;
    
    size_t count = it->second.size();
    spdlog::info("[BATCH FLUSH] topic={} count={} (window={}s)", 
                 topic, count, window_.count());
    
    // Aggregate batch metrics
    uint64_t total_bytes = 0;
    uint32_t min_id = UINT32_MAX;
    uint32_t max_id = 0;
    
    for (const auto& evt : it->second) {
        total_bytes += evt.body.size();
        min_id = std::min(min_id, evt.header.id);
        max_id = std::max(max_id, evt.header.id);
    }
    
    double avg_bytes = total_bytes / static_cast<double>(count);
    spdlog::debug("  [BATCH] Aggregated: {} events, {} bytes, avg {:.1f}b, id_range [{}, {}]",
                  count, total_bytes, avg_bytes, min_id, max_id);
    
    // TODO: Persist to storage_engine in batch (one write, not per-event)
    // This is a placeholder - integrate with storage engine for bulk insert
    
    it->second.clear();
}
