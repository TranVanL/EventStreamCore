#include "eventprocessor/event_processor.hpp"
#include "utils/thread_affinity.hpp"
#include <chrono>

RealtimeProcessor::RealtimeProcessor() {}

RealtimeProcessor::~RealtimeProcessor() noexcept {
    stop();
    spdlog::info("[DESTRUCTOR] RealtimeProcessor destroyed successfully");
}

void RealtimeProcessor::start() {
    spdlog::info("RealtimeProcessor started.");
}

void RealtimeProcessor::stop() {
    spdlog::info("RealtimeProcessor stopped.");
}



void RealtimeProcessor::process(const EventStream::Event& event) {
    auto start_time = std::chrono::high_resolution_clock::now();
    constexpr int MAX_PROCESSING_MS = 5;
    
    auto &m = MetricRegistry::getInstance().getMetrics(name());

    if (handle(event)) {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto total_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        auto total_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        
        if (total_elapsed_ms > MAX_PROCESSING_MS) {
            m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("DROPPED event id {} - processing latency {}ms exceeds SLA 5ms", event.header.id, total_elapsed_ms);
            return;
        }
        
        // Track latency metrics for adaptive tuning
        m.total_processing_time_ns.fetch_add(total_elapsed_ns, std::memory_order_relaxed);
        uint64_t current_max = m.max_processing_time_ns.load();
        while (total_elapsed_ns > current_max && 
               !m.max_processing_time_ns.compare_exchange_weak(current_max, total_elapsed_ns, 
                                                               std::memory_order_relaxed)) {
            current_max = m.max_processing_time_ns.load();
        }

        m.event_count_for_avg.fetch_add(1, std::memory_order_relaxed);
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        
        // Update last event timestamp (for stale detection)
        MetricRegistry::getInstance().updateEventTimestamp(name());
    } else {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        spdlog::error("RealtimeProcessor failed to process event id {}", event.header.id);
    }
}

bool RealtimeProcessor::handle(const EventStream::Event& event) {
    // Alert detection for CRITICAL/HIGH priority events
    
    if (event.body.size() > 1024) {
        
        spdlog::warn("Alert: Event id {} has large body size: {}", event.header.id, event.body.size());
        return true;
    }

    if (event.topic == "sensor/temperature" && !event.body.empty()) {
        uint8_t temp = event.body[0];
        if (temp > 100) {
            spdlog::warn("Critical alert: Temperature {}C for event id {}", temp, event.header.id);
            return true;
        }
    }

    // Accept all other events
    return true;
}

