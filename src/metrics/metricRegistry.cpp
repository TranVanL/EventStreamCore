#include "metrics/metricRegistry.hpp"
#include <chrono>

MetricRegistry& MetricRegistry::getInstance() {
    static MetricRegistry instance;
    return instance;
}

void MetricRegistry::setHealthThresholds(const HealthThresholds& t) {
    std::lock_guard<std::mutex> lock(mtx_config_);
    thresholds_ = t;
}

const MetricRegistry::HealthThresholds& MetricRegistry::getHealthThresholds() const {
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

MetricSnapshot MetricRegistry::buildSnapshot(Metrics& m, const HealthThresholds& t, uint64_t ts) {
    auto proc = m.total_events_processed.load(std::memory_order_relaxed);
    auto drop = m.total_events_dropped.load(std::memory_order_relaxed);
    auto err = m.total_events_errors.load(std::memory_order_relaxed);
    auto last_ts = m.last_event_timestamp_ms.load(std::memory_order_relaxed);
    auto depth = m.current_queue_depth.load(std::memory_order_relaxed);
    
    auto health = checkHealth(proc, drop, err, last_ts, depth, ts, t);
    m.health_status.store(static_cast<uint8_t>(health), std::memory_order_relaxed);
    
    MetricSnapshot snap{};
    snap.total_events_processed = proc;
    snap.total_events_dropped = drop;
    snap.total_events_alerted = m.total_events_alerted.load(std::memory_order_relaxed);
    snap.total_events_errors = err;
    snap.total_events_skipped = m.total_events_skipped.load(std::memory_order_relaxed);
    snap.total_retries = m.total_retries.load(std::memory_order_relaxed);
    snap.total_events_enqueued = m.total_events_enqueued.load(std::memory_order_relaxed);
    snap.total_events_dequeued = m.total_events_dequeued.load(std::memory_order_relaxed);
    snap.total_events_blocked = m.total_events_blocked.load(std::memory_order_relaxed);
    snap.total_overflow_drops = m.total_overflow_drops.load(std::memory_order_relaxed);
    snap.total_processing_time_ns = m.total_processing_time_ns.load(std::memory_order_relaxed);
    snap.max_processing_time_ns = m.max_processing_time_ns.load(std::memory_order_relaxed);
    snap.event_count_for_avg = m.event_count_for_avg.load(std::memory_order_relaxed);
    snap.last_event_timestamp_ms = last_ts;
    snap.current_queue_depth = depth;
    snap.health_status = health;
    return snap;
}

HealthStatus MetricRegistry::checkHealth(uint64_t proc, uint64_t drop, uint64_t err, uint64_t last_ts, uint64_t depth, uint64_t ts, const HealthThresholds& t) {
    // UNHEALTHY checks
    if (last_ts > 0 && (ts - last_ts) > t.stale_timeout_ms) return HealthStatus::UNHEALTHY;
    if (depth > t.unhealthy_queue_depth) return HealthStatus::UNHEALTHY;
    if (proc > 0 && (err * 100) / proc >= t.unhealthy_error_rate_percent) return HealthStatus::UNHEALTHY;
    
    uint64_t total = proc + drop;
    if (total > 0 && (drop * 100) / total >= t.unhealthy_drop_rate_percent) return HealthStatus::UNHEALTHY;
    
    // DEGRADED checks
    if (depth > t.degraded_queue_depth) return HealthStatus::DEGRADED;
    if (total > 0 && (drop * 100) / total >= t.degraded_drop_rate_percent) return HealthStatus::DEGRADED;
    
    return HealthStatus::HEALTHY;
}
MetricRegistry::AggregateMetrics MetricRegistry::getAggregateMetrics() const {
    std::lock_guard<std::mutex> lock(mtx_metrics_);
    
    AggregateMetrics aggregate{};
    
    // Tính toán các metrics aggregate từ tất cả processors
    uint64_t total_processed = 0;
    uint64_t total_dropped = 0;
    
    for (const auto& [name, metrics] : metrics_map_) {
        aggregate.total_queue_depth += metrics.current_queue_depth.load(std::memory_order_relaxed);
        aggregate.total_dropped += metrics.total_events_dropped.load(std::memory_order_relaxed);
        aggregate.total_processed += metrics.total_events_processed.load(std::memory_order_relaxed);
        
        uint64_t max_lat = metrics.max_processing_time_ns.load(std::memory_order_relaxed);
        if (max_lat > aggregate.max_processor_latency_ns) {
            aggregate.max_processor_latency_ns = max_lat;
        }
    }
    
    // Tính drop rate
    uint64_t total = aggregate.total_processed + aggregate.total_dropped;
    if (total > 0) {
        aggregate.aggregate_drop_rate_percent = (aggregate.total_dropped * 100.0) / total;
    }
    
    return aggregate;
}