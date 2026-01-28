#include "admin/AdminLoop.hpp"
#include "admin/ControlDecision.hpp"

using namespace EventStream;

Admin::Admin(ProcessManager& pm) 
    : process_manager_(pm), 
      control_plane_(std::make_unique<ControlPlane>()) {}

Admin::~Admin() noexcept {
    stop();
}

void Admin::start() {
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&Admin::loop, this);
}

void Admin::stop() {
    running_.store(false, std::memory_order_release);
    sleep_cv_.notify_all();  // Wake up sleeping thread immediately
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void Admin::loop() {
    using namespace std::chrono_literals;
    auto& registry = MetricRegistry::getInstance();
    
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
        
        // Simple control decision
        auto decision = control_plane_->evaluateMetrics(
            total_queue, total_processed, total_dropped, 0
        );
        control_plane_->executeDecision(decision, pipeline_state_);
        
        // Report metrics to logs
        reportMetrics(snaps);
    }
}

void Admin::reportMetrics(const std::unordered_map<std::string, MetricSnapshot>& snapshots) {
    spdlog::info("========== PROCESSOR STATUS (Health Check) ==========");
    
    uint64_t total_processed = 0;
    uint64_t total_dropped = 0;
    int healthy = 0, unhealthy = 0;
    
    for (const auto& [name, snap] : snapshots) {
        total_processed += snap.total_events_processed;
        total_dropped += snap.total_events_dropped;
        
        const char* status = (snap.health_status == HealthStatus::HEALTHY) ? " HEALTHY" : " UNHEALTHY";
        uint64_t drop_rate = snap.get_drop_rate_percent();
        
        spdlog::info("[{}] {} | Processed: {} | Dropped: {} ({:.1f}%) | Queue: {}", 
            status, name, snap.total_events_processed, snap.total_events_dropped, 
            static_cast<double>(drop_rate), snap.current_queue_depth);
        
        if (snap.health_status == HealthStatus::HEALTHY) healthy++;
        else unhealthy++;
    }
    
    spdlog::info("===== AGGREGATE: {} OK, {} ALERTS =====", healthy, unhealthy);
    spdlog::info("Total Processed: {} | Total Dropped: {} ({:.1f}%)", 
        total_processed, total_dropped, 
        total_processed > 0 ? (total_dropped * 100.0 / total_processed) : 0.0);
}