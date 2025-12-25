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

        // Get snapshots (health status calculated & updated inside)
        auto snapshots = registry.getSnapshots();

        // Make control plane decisions based on metrics
        makeControlDecisions(snapshots);

        // Report metrics
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
// Private: Control Plane Decisions
// ============================================================================

void Admin::makeControlDecisions(const std::unordered_map<std::string, MetricSnapshot>& snapshots) {
    for (const auto& [name, snap] : snapshots) {
        auto decision = control_plane_.MakeDecision(snap);

        const char* state_str = decision.state == FailureState::Healthy ? "HEALTHY" :
                               decision.state == FailureState::Overloaded ? "OVERLOADED" :
                               decision.state == FailureState::LatencySpike ? "LATENCY_SPIKE" : "ERROR_BURST";
        const char* action_str = decision.action == ControlAction::NoAction ? "NoAction" :
                                decision.action == ControlAction::PauseTransactions ? "PauseTransactions" :
                                decision.action == ControlAction::ResumeTransactions ? "ResumeTransactions" :
                                decision.action == ControlAction::DropBatchEvents ? "DropBatchEvents" : "PushDLQ";

        if (decision.state != FailureState::Healthy) {
            spdlog::warn("[CONTROL] {} | {} | {} | {}", name, state_str, action_str, decision.reason);
        } else {
            spdlog::debug("[CONTROL] {} | {} | {} | {}", name, state_str, action_str, decision.reason);
        }

        // Apply control actions to processors
        if (decision.action == ControlAction::PauseTransactions) {
            process_manager_.pauseTransactions();
        } else if (decision.action == ControlAction::ResumeTransactions) {
            process_manager_.resumeTransactions();
        } else if (decision.action == ControlAction::DropBatchEvents) {
            process_manager_.dropBatchEvents();
        }
    }
}
