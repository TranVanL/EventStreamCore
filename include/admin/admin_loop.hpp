#pragma once 

#include <atomic>
#include <thread>
#include <unordered_map>
#include <memory>
#include <spdlog/spdlog.h>

#include "eventprocessor/processManager.hpp"
#include "metrics/metricRegistry.hpp"
#include "control/PipelineState.hpp"
#include "control/Control_plane.hpp"
#include "admin/ControlDecision.hpp"

namespace EventStream {

class Admin {
public:
    explicit Admin(ProcessManager& pm);
    ~Admin() noexcept;

    void start();
    void stop();

private:
    void loop();
    void reportMetrics(const std::unordered_map<std::string, MetricSnapshot>& snapshots);

    ProcessManager& process_manager_;
    PipelineStateManager pipeline_state_;  
    std::unique_ptr<ControlPlane> control_plane_;
    
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
};

} // namespace EventStream