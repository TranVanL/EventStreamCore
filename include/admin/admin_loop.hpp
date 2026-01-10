#pragma once 

#include <atomic>
#include <thread>
#include <unordered_map>
#include <spdlog/spdlog.h>

#include "eventprocessor/processManager.hpp"
#include "metrics/metricRegistry.hpp"
#include "control/PipelineState.hpp"

/**
 * CONTROL PLANE: AdminLoop decision maker (THE BRAIN)
 * - Đọc metrics định kỳ
 * - Quyết định thay đổi PipelineState
 * - Là thành phần DUY NHẤT có quyền thay đổi state
 * - Workers chỉ đọc state, không thay đổi
 */
class Admin {
public:
    explicit Admin(ProcessManager& pm);
    ~Admin() noexcept;

    void start();
    void stop();

private:
    void loop();
    void reportMetrics(const std::unordered_map<std::string, MetricSnapshot>& snapshots);
    
    /**
     * Lõi quyết định: đọc metrics → quyết định state
     * Gọi định kỳ từ loop()
     */
    void control_tick();

    ProcessManager& process_manager_;
    PipelineStateManager pipeline_state_;  // Quản lý state pipeline - singleton-like
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    
    // Threshold cho state transitions (có thể load từ config)
    static constexpr uint64_t CRITICAL_QUEUE_DEPTH = 10000;
    static constexpr uint64_t HIGH_QUEUE_DEPTH = 5000;
    static constexpr double CRITICAL_DROP_RATE = 5.0;  // 5%
    static constexpr double HIGH_DROP_RATE = 2.0;      // 2%
    static constexpr uint64_t CRITICAL_LATENCY_MS = 500;
};