#include <eventstream/core/admin/admin_loop.hpp>
#include <eventstream/core/admin/control_decision.hpp>

using namespace EventStream;

Admin::Admin(ProcessManager& pm) 
    : process_manager_(pm), 
      control_plane_(std::make_unique<ControlPlane>()) {
    spdlog::info("[Admin] Initialized with ControlPlane");
}

Admin::~Admin() noexcept {
    spdlog::info("[Admin] Shutting down...");
    stop();
}

void Admin::start() {
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&Admin::loop, this);
    spdlog::info("[Admin] Started monitoring loop (interval: 10s)");
}

void Admin::stop() {
    running_.store(false, std::memory_order_release);
    sleep_cv_.notify_all();  // Wake up sleeping thread immediately
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    spdlog::info("[Admin] Stopped");
}

void Admin::loop() {
    using namespace std::chrono_literals;
    auto& registry = MetricRegistry::getInstance();
    
    // Track consecutive unhealthy cycles for escalation
    int consecutive_unhealthy = 0;
    
    while (running_.load(std::memory_order_acquire)) {
        // Interruptible sleep: wait for 10s OR until stop() is called
        {
            std::unique_lock<std::mutex> lock(sleep_mutex_);
            sleep_cv_.wait_for(lock, 10s, [this]() { 
                return !running_.load(std::memory_order_acquire); 
            });
        }
        
        // Check if we were woken up due to shutdown
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        
        // Collect metrics from all processors
        auto snaps = registry.getSnapshots();
        
        // Sum key metrics
        uint64_t total_queue = 0;
        uint64_t total_processed = 0;
        uint64_t total_dropped = 0;
        
        for (const auto& [name, snap] : snaps) {
            total_queue += snap.current_queue_depth;
            total_processed += snap.total_events_processed;
            total_dropped += snap.total_events_dropped;
        }
        
        // Evaluate metrics and get decision
        auto decision = control_plane_->evaluateMetrics(
            total_queue, total_processed, total_dropped, 0
        );
        
        // Execute decision on pipeline state
        control_plane_->executeDecision(decision, pipeline_state_);
        
        // Execute additional actions based on decision
        executeControlAction(decision);
        
        // Track consecutive unhealthy for escalation
        if (decision.reason != FailureState::HEALTHY) {
            consecutive_unhealthy++;
            if (consecutive_unhealthy >= 3) {
                spdlog::error("[Admin] System unhealthy for {} consecutive cycles!", 
                              consecutive_unhealthy);
            }
        } else {
            if (consecutive_unhealthy > 0) {
                spdlog::info("[Admin] System recovered after {} unhealthy cycles", 
                             consecutive_unhealthy);
            }
            consecutive_unhealthy = 0;
        }
        
        // Report metrics to logs
        reportMetrics(snaps, decision);
    }
}

void Admin::executeControlAction(const EventControlDecision& decision) {
    switch (decision.action) {
        case ControlAction::PAUSE_PROCESSOR:
            // Pause transactional processing to reduce load
            process_manager_.pauseTransactions();
            spdlog::warn("[Admin] ACTION: Paused TransactionalProcessor");
            break;
            
        case ControlAction::DROP_BATCH:
            // Proactively drop batch events to DLQ
            process_manager_.dropBatchEvents();
            spdlog::warn("[Admin] ACTION: Dropping batch events to DLQ");
            break;
            
        case ControlAction::PUSH_DLQ:
            // Emergency: drop batch and pause
            process_manager_.dropBatchEvents();
            process_manager_.pauseTransactions();
            spdlog::error("[Admin] ACTION: EMERGENCY - Dropped batch and paused processing");
            break;
            
        case ControlAction::DRAIN:
            // Graceful shutdown preparation
            spdlog::info("[Admin] ACTION: Draining pipeline...");
            break;
            
        case ControlAction::RESUME:
            // Resume normal operation
            process_manager_.resumeTransactions();
            process_manager_.resumeBatchEvents();
            break;
            
        case ControlAction::NONE:
        default:
            // No action needed
            break;
    }
}

void Admin::reportMetrics(const std::unordered_map<std::string, MetricSnapshot>& snapshots,
                          const EventControlDecision& decision) {
    // Only log detailed report every cycle, but use appropriate log level
    auto log_level = (decision.reason == FailureState::HEALTHY) 
        ? spdlog::level::info 
        : spdlog::level::warn;
    
    spdlog::log(log_level, "╔════════════════════════════════════════════════════════════╗");
    spdlog::log(log_level, "║              SYSTEM HEALTH REPORT                          ║");
    spdlog::log(log_level, "╠════════════════════════════════════════════════════════════╣");
    
    uint64_t total_processed = 0;
    uint64_t total_dropped = 0;
    uint64_t total_queue = 0;
    int healthy = 0, unhealthy = 0;
    
    for (const auto& [name, snap] : snapshots) {
        total_processed += snap.total_events_processed;
        total_dropped += snap.total_events_dropped;
        total_queue += snap.current_queue_depth;
        
        const char* status = (snap.health_status == HealthStatus::HEALTHY) ? "✓" : "✗";
        double drop_rate = static_cast<double>(snap.get_drop_rate_percent());
        
        spdlog::log(log_level, "║ [{}] {:20} │ Proc: {:8} │ Drop: {:5} ({:5.1f}%) │ Q: {:5} ║", 
            status, name, snap.total_events_processed, snap.total_events_dropped, 
            drop_rate, snap.current_queue_depth);
        
        if (snap.health_status == HealthStatus::HEALTHY) healthy++;
        else unhealthy++;
    }
    
    spdlog::log(log_level, "╠════════════════════════════════════════════════════════════╣");
    
    // Pipeline state
    const char* state_str = PipelineStateManager::toString(pipeline_state_.getState());
    const char* decision_str = EventControlDecision::actionString(decision.action);
    const char* health_str = EventControlDecision::failureStateString(decision.reason);
    
    spdlog::log(log_level, "║ Pipeline: {:10} │ Decision: {:15} │ Health: {:8} ║",
                state_str, decision_str, health_str);
    
    double total_drop_rate = (total_processed + total_dropped) > 0 
        ? (total_dropped * 100.0 / (total_processed + total_dropped)) 
        : 0.0;
    
    spdlog::log(log_level, "╠════════════════════════════════════════════════════════════╣");
    spdlog::log(log_level, "║ AGGREGATE: {} OK, {} ALERTS │ Total Q: {:6} │ Drop: {:5.1f}%     ║",
                healthy, unhealthy, total_queue, total_drop_rate);
    spdlog::log(log_level, "╚════════════════════════════════════════════════════════════╝");
}