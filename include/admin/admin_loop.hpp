#pragma once 

#include <atomic>
#include <spdlog/spdlog.h>
#include <thread>

#include "eventprocessor/processManager.hpp"
#include "metrics/metricRegistry.hpp"

/**
 * AdminLoop: CONTROL PLANE decision maker
 * - Reads metric snapshots periodically
 * - Analyzes health status
 * - Makes decisions based on health
 */
class Admin {
public:
    explicit Admin(ProcessManager& pm);
    ~Admin() noexcept;

    void start() {
        running_.store(true, std::memory_order_release);
        worker_thread_ = std::thread(&Admin::loop, this);
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
    
private:
    ProcessManager& process_manager_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    
    void loop();
};