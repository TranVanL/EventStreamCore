#pragma once 

#include <atomic>
#include <thread>
#include <unordered_map>
#include <spdlog/spdlog.h>

#include "eventprocessor/processManager.hpp"
#include "metrics/metricRegistry.hpp"
#include "control/control_plane.hpp"

/**
 * CONTROL PLANE: AdminLoop decision maker
 * - Reads metric snapshots periodically
 * - Analyzes health status
 * - Makes decisions via control_plane
 * - Reports metrics and control actions
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
    void makeControlDecisions(const std::unordered_map<std::string, MetricSnapshot>& snapshots);

    ProcessManager& process_manager_;
    ControlPlane control_plane_;    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};