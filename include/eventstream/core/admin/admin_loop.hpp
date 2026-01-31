#pragma once 

#include <atomic>
#include <thread>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <spdlog/spdlog.h>

#include <eventstream/core/processor/process_manager.hpp>
#include <eventstream/core/metrics/registry.hpp>
#include <eventstream/core/control/pipeline_state.hpp>
#include <eventstream/core/control/control_plane.hpp>
#include <eventstream/core/admin/control_decision.hpp>

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
    
    // For interruptible sleep during shutdown
    mutable std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;
};

} // namespace EventStream