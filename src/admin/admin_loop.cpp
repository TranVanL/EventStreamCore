#include "admin/admin_loop.hpp"
#include "admin/ControlDecision.hpp"

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
    
    // Create snapshot for evaluation (Day 22 compatible)
    MetricSnapshot snap{};
    snap.current_queue_depth = agg.total_queue_depth;
    snap.total_processing_time_ns = agg.max_processor_latency_ns;
    snap.total_events_dropped = agg.total_dropped;
    snap.total_events_processed = agg.total_processed;
    snap.event_count_for_avg = agg.total_processed;
    
    // STEP 1: Evaluate metrics and make decision (pure function - Day 23)
    EventStream::EventControlDecision decision = evaluateSnapshot(snap);
    
    // STEP 2: Execute the decision (may have multiple effects - Day 23)
    executeDecision(decision);
    
    // STEP 3: Update PipelineState based on decision (Day 22 backward compatible)
    PipelineState newState = PipelineState::RUNNING;
    if (decision.action == EventStream::ControlAction::DROP_BATCH ||
        decision.action == EventStream::ControlAction::PUSH_DLQ) {
        newState = PipelineState::DROPPING;
    } else if (decision.action == EventStream::ControlAction::DRAIN) {
        newState = PipelineState::DRAINING;
    } else if (decision.action == EventStream::ControlAction::PAUSE_PROCESSOR) {
        newState = PipelineState::PAUSED;
    }
    
    pipeline_state_.setState(newState);
}
// ============================================================================
// Day 23: Control Decision Logic
// ============================================================================

EventStream::EventControlDecision Admin::evaluateSnapshot(const MetricSnapshot& snap) {
    // Pure function: MetricsSnapshot → EventControlDecision
    // Can be tested independently without any dependencies
    
    uint64_t queue_depth = snap.current_queue_depth;
    uint64_t latency_ms = snap.get_avg_latency_ns() / 1'000'000;
    double drop_rate = snap.get_drop_rate_percent();
    
    // CRITICAL checks (highest priority)
    if (queue_depth > CRITICAL_QUEUE_DEPTH) {
        return {
            EventStream::ControlAction::DROP_BATCH,
            EventStream::FailureState::CRITICAL,
            "Queue depth " + std::to_string(queue_depth) + 
            " exceeds critical limit " + std::to_string(CRITICAL_QUEUE_DEPTH)
        };
    }
    
    if (drop_rate > CRITICAL_DROP_RATE) {
        return {
            EventStream::ControlAction::PUSH_DLQ,
            EventStream::FailureState::CRITICAL,
            "Drop rate " + std::to_string(static_cast<int>(drop_rate)) + 
            "% exceeds critical threshold " + std::to_string(static_cast<int>(CRITICAL_DROP_RATE)) + "%"
        };
    }
    
    if (latency_ms > CRITICAL_LATENCY_MS) {
        return {
            EventStream::ControlAction::DRAIN,
            EventStream::FailureState::CRITICAL,
            "Latency " + std::to_string(latency_ms) + 
            "ms exceeds critical threshold " + std::to_string(CRITICAL_LATENCY_MS) + "ms"
        };
    }
    
    // DEGRADED checks (medium priority)
    if (queue_depth > HIGH_QUEUE_DEPTH) {
        return {
            EventStream::ControlAction::PAUSE_PROCESSOR,
            EventStream::FailureState::DEGRADED,
            "Queue depth " + std::to_string(queue_depth) + 
            " exceeds high watermark " + std::to_string(HIGH_QUEUE_DEPTH)
        };
    }
    
    if (drop_rate > HIGH_DROP_RATE) {
        return {
            EventStream::ControlAction::PAUSE_PROCESSOR,
            EventStream::FailureState::DEGRADED,
            "Drop rate " + std::to_string(static_cast<int>(drop_rate)) + 
            "% exceeds threshold " + std::to_string(static_cast<int>(HIGH_DROP_RATE)) + "%"
        };
    }
    
    // HEALTHY (no action needed)
    return {
        EventStream::ControlAction::NONE,
        EventStream::FailureState::HEALTHY,
        "System healthy"
    };
}

void Admin::executeDecision(const EventStream::EventControlDecision& decision) {
    // Execute the formal decision - may take multiple actions
    
    spdlog::warn("[CONTROL DECISION] Action={} Reason={} Details={}",
                 EventStream::EventControlDecision::actionString(decision.action),
                 EventStream::EventControlDecision::failureStateString(decision.reason),
                 decision.details);
    
    switch (decision.action) {
    case EventStream::ControlAction::NONE:
        spdlog::debug("[EXECUTE] No action - system healthy");
        break;
        
    case EventStream::ControlAction::PAUSE_PROCESSOR:
        spdlog::warn("[EXECUTE] Pausing TransactionalProcessor");
        process_manager_.pauseTransactions();
        break;
        
    case EventStream::ControlAction::DRAIN:
        spdlog::warn("[EXECUTE] Draining TransactionalProcessor");
        process_manager_.resumeBatchEvents();
        // Drain: allow 100ms for pending events to settle (non-blocking interval)
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        process_manager_.pauseTransactions();
        break;
        
    case EventStream::ControlAction::DROP_BATCH:
        spdlog::error("[EXECUTE] Dropping batch from queue");
        process_manager_.dropBatchEvents();
        break;
        
    case EventStream::ControlAction::PUSH_DLQ:
        spdlog::error("[EXECUTE] Pushing failed events to DLQ");
        // Log DLQ statistics
        {
            auto& dlq = process_manager_.getEventBus().getDLQ();
            spdlog::error("[EXECUTE] DLQ Total Dropped: {}", dlq.size());
        }
        break;
        break;
        
    case EventStream::ControlAction::RESUME:
        spdlog::info("[EXECUTE] Resuming TransactionalProcessor");
        process_manager_.resumeTransactions();
        break;
        
    default:
        spdlog::error("[EXECUTE] Unknown action: {}", 
                      static_cast<int>(decision.action));
        break;
    }
}