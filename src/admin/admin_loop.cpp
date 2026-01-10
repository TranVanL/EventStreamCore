#include "admin/admin_loop.hpp"

// ============================================================================
// Constructor/Destructor
// ============================================================================

Admin::Admin(ProcessManager& pm) : process_manager_(pm) {}

Admin::~Admin() noexcept {
    spdlog::info("[DESTRUCTOR] Admin loop shutting down...");
    stop();
    spdlog::info("[DESTRUCTOR] Admin loop shut down successfully");
}

// ============================================================================
// Thread Control
// ============================================================================

void Admin::start() {
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&Admin::loop, this);
}

void Admin::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

// ============================================================================
// Main Loop: Periodic Health Check & Reporting
// ============================================================================

void Admin::loop() {
    using namespace std::chrono_literals;
    auto& registry = MetricRegistry::getInstance();

    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(10s);

        // Lõi quyết định: cập nhật PipelineState dựa trên aggregate metrics
        control_tick();

        // Lấy snapshots để report (riêng biệt từ control decisions)
        auto snapshots = registry.getSnapshots();
        reportMetrics(snapshots);
    }
}

// ============================================================================
// Private: Metrics Reporting
// ============================================================================

void Admin::reportMetrics(const std::unordered_map<std::string, MetricSnapshot>& snapshots) {
    spdlog::info("============================= HEALTH =============================");
    
    int ok = 0, warn = 0, crit = 0;
    
    for (const auto& [name, snap] : snapshots) {
        const char* str = snap.health_status == HealthStatus::HEALTHY ? "OK" :
                         snap.health_status == HealthStatus::DEGRADED ? "WARN" : "CRIT";
        
        spdlog::info("  [{}] {} | Events: {} | Drop: {}%", 
            str, name, snap.total_events_processed, snap.get_drop_rate_percent());
        
        if (snap.health_status == HealthStatus::HEALTHY) ok++;
        else if (snap.health_status == HealthStatus::DEGRADED) warn++;
        else crit++;
    }
    
    spdlog::info("  Summary: {} OK, {} WARN, {} CRIT", ok, warn, crit);
    spdlog::info("===================================================================");
}

// ============================================================================
// NGÔN NGỮ CHUNG: Pipeline State Machine Control
// ============================================================================

void Admin::control_tick() {
    auto& registry = MetricRegistry::getInstance();
    auto agg = registry.getAggregateMetrics();
    
    PipelineState newState = PipelineState::RUNNING;
    
    // Quyết định state dựa trên metrics aggregate
    // Thứ tự kiểm tra: từ critical → bình thường
    
    if (agg.total_queue_depth > CRITICAL_QUEUE_DEPTH) {
        // Queue quá sâu - EMERGENCY
        newState = PipelineState::DROPPING;
        spdlog::warn("[CONTROL_TICK] CRITICAL: queue_depth={} > {}", 
                     agg.total_queue_depth, CRITICAL_QUEUE_DEPTH);
    }
    else if (agg.aggregate_drop_rate_percent > CRITICAL_DROP_RATE) {
        // Drop rate quá cao - chuyển sang DROPPING
        newState = PipelineState::DROPPING;
        spdlog::warn("[CONTROL_TICK] CRITICAL: drop_rate={:.1f}% > {:.1f}%", 
                     agg.aggregate_drop_rate_percent, CRITICAL_DROP_RATE);
    }
    else if (agg.max_processor_latency_ns > (CRITICAL_LATENCY_MS * 1'000'000)) {
        // Latency spike - DRAINING để xả backlog
        newState = PipelineState::DRAINING;
        spdlog::warn("[CONTROL_TICK] HIGH: latency={}ms > {}ms", 
                     agg.max_processor_latency_ns / 1'000'000, CRITICAL_LATENCY_MS);
    }
    else if (agg.total_queue_depth > HIGH_QUEUE_DEPTH) {
        // Queue depth trung bình - PAUSED
        newState = PipelineState::PAUSED;
        spdlog::debug("[CONTROL_TICK] MEDIUM: queue_depth={} > {}", 
                      agg.total_queue_depth, HIGH_QUEUE_DEPTH);
    }
    else if (agg.aggregate_drop_rate_percent > HIGH_DROP_RATE) {
        // Drop rate trung bình - PAUSED
        newState = PipelineState::PAUSED;
        spdlog::debug("[CONTROL_TICK] MEDIUM: drop_rate={:.1f}% > {:.1f}%", 
                      agg.aggregate_drop_rate_percent, HIGH_DROP_RATE);
    }
    else {
        // Bình thường - RUNNING
        newState = PipelineState::RUNNING;
    }
    
    // Cập nhật state (chỉ thay đổi khi khác)
    pipeline_state_.setState(newState);
}
