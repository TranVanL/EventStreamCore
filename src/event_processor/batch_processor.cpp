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
            return;
        }
        
        if (now - last >= window_) {
            flush(event.topic);
            last = now;
        }
    }
}

void BatchProcessor::flush(const std::string& topic) {
    auto it = buckets_.find(topic);
    if (it == buckets_.end() || it->second.empty())
        return;
    
    size_t count = it->second.size();
    spdlog::info("[BATCH FLUSH] topic={} count={} (window={}s)", 
                 topic, count, window_.count());
    
    // TODO: Aggregate and persist batch
    // - Sum/Avg/Count metrics
    // - Persist to storage in one batch write
    // - Update analytics
    
    it->second.clear();
}
