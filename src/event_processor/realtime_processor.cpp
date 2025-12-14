#include "eventprocessor/event_processor.hpp"
#include <chrono>


// RealtimeProcessor Contract
// --------------------------
// Input:
//   - Events from EventBusMulti::REALTIME
//   - Priority: HIGH / CRITICAL
//
// Guarantees:
//   - Best-effort processing
//   - Non-blocking (no disk, no network)
//   - Target latency < 5ms / event
//
// Failure handling:
//   - Drop event on overload or error
//   - Log warning only
//
// Typical use cases:
//   - Alerting
//   - Threshold breach
//   - Fast signal forwarding



RealtimeProcessor::RealtimeProcessor() {
}

RealtimeProcessor::~RealtimeProcessor() noexcept {
    spdlog::info("[DESTRUCTOR] RealtimeProcessor being destroyed...");
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

    spdlog::info("RealtimeProcessor processing event id: {} topic: {} size: {}", event.header.id, event.topic, event.body.size());

    if (handle(event)) {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto total_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        auto total_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        
        if (total_elapsed_ms > MAX_PROCESSING_MS) {
            m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
            dropped_events_.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("DROPPED event id {} - processing latency {}ms exceeds SLA 5ms", event.header.id, total_elapsed_ms);
            return;
        }
        
        // Track latency metrics
        m.total_processing_time_ns.fetch_add(total_elapsed_ns, std::memory_order_relaxed);
        uint64_t current_max = m.max_processing_time_ns.load();
        while (total_elapsed_ns > current_max && 
               !m.max_processing_time_ns.compare_exchange_weak(current_max, total_elapsed_ns, 
                                                               std::memory_order_relaxed)) {
            current_max = m.max_processing_time_ns.load();
        }
        m.event_count_for_avg.fetch_add(1, std::memory_order_relaxed);
        
        m.total_events_processed.fetch_add(1, std::memory_order_relaxed);
        processed_events_.fetch_add(1, std::memory_order_relaxed);
    } else {
        m.total_events_dropped.fetch_add(1, std::memory_order_relaxed);
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        spdlog::error("RealtimeProcessor failed to process event id {}", event.header.id);
    }
}

bool RealtimeProcessor::handle(const EventStream::Event& event) {
    // Alert detection for CRITICAL/HIGH priority events
    
    if (event.body.size() > 1024) {
        alert_events_.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("Alert: Event id {} has large body size: {}", event.header.id, event.body.size());
        return true;
    }

    if (event.topic == "sensor/temperature" && !event.body.empty()) {
        uint8_t temp = event.body[0];
        if (temp > 100) {
            alert_events_.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("Critical alert: Temperature {}C for event id {}", temp, event.header.id);
            return true;
        }
    }

    if (event.topic == "system/heartbeat") {
        spdlog::debug("Heartbeat event received: id {}", event.header.id);
        return true;
    }

    if (event.topic == "network/latency" && !event.body.empty()) {
        spdlog::debug("Network latency event received: id {}", event.header.id);
        return true;
    }

    spdlog::debug("RealtimeProcessor handled event id {} from topic {}", event.header.id, event.topic);
    return true;
}

