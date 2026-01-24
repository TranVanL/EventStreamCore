#include "metrics/metricRegistry.hpp"
#include <chrono>

MetricRegistry& MetricRegistry::getInstance() {
    static MetricRegistry instance;
    return instance;
}

void MetricRegistry::setThresholds(const EventStream::ControlThresholds& t) {
    std::lock_guard<std::mutex> lock(mtx_config_);
    thresholds_ = t;
}

const EventStream::ControlThresholds& MetricRegistry::getThresholds() const {
    std::lock_guard<std::mutex> lock(mtx_config_);
    return thresholds_;
}

Metrics& MetricRegistry::getMetrics(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_metrics_);
    return metrics_map_[name];
}

std::unordered_map<std::string, MetricSnapshot> MetricRegistry::getSnapshots() {
    std::lock_guard<std::mutex> lock(mtx_metrics_);
    auto ts = now();
    auto t = thresholds_;
    
    std::unordered_map<std::string, MetricSnapshot> snaps;
    snaps.reserve(metrics_map_.size());
    for (auto& [name, m] : metrics_map_) {
        snaps[name] = buildSnapshot(m, t, ts);
    }
    return snaps;
}

std::optional<MetricSnapshot> MetricRegistry::getSnapshot(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_metrics_);
    auto it = metrics_map_.find(name);
    if (it == metrics_map_.end()) return std::nullopt;
    return buildSnapshot(it->second, thresholds_, now());
}

void MetricRegistry::updateEventTimestamp(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_metrics_);
    auto it = metrics_map_.find(name);
    if (it != metrics_map_.end()) {
        it->second.last_event_timestamp_ms.store(now(), std::memory_order_relaxed);
    }
}

uint64_t MetricRegistry::now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

MetricSnapshot MetricRegistry::buildSnapshot(Metrics& m, const EventStream::ControlThresholds& t, uint64_t ts) {
    auto proc = m.total_events_processed.load(std::memory_order_relaxed);
    auto drop = m.total_events_dropped.load(std::memory_order_relaxed);
    auto depth = m.current_queue_depth.load(std::memory_order_relaxed);
    
    // Simplified health check - use thresholds only
    HealthStatus health = (depth > t.max_queue_depth || 
                          (proc > 0 && (drop * 100) / proc > static_cast<uint64_t>(t.max_drop_rate)))
        ? HealthStatus::UNHEALTHY 
        : HealthStatus::HEALTHY;
    
    m.health_status.store(static_cast<uint8_t>(health), std::memory_order_relaxed);
    
    MetricSnapshot snap{};
    snap.total_events_processed = proc;
    snap.total_events_dropped = drop;
    snap.current_queue_depth = depth;
    snap.health_status = health;
    return snap;
}

HealthStatus MetricRegistry::checkHealth(uint64_t proc, uint64_t drop, uint64_t depth, uint64_t last_ts, uint64_t stale_timeout_ms, uint64_t now_ms) {
    return HealthStatus::HEALTHY;
}