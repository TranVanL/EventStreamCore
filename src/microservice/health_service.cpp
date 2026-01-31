/**
 * @file health_service.cpp
 * @brief Health service implementation
 */

#include <eventstream/microservice/health_service.hpp>

namespace eventstream::microservice {

HealthService::HealthService() = default;

bool HealthService::is_alive() const {
    return true;  // If we're running, we're alive
}

bool HealthService::is_ready() const {
    return ready_.load(std::memory_order_relaxed);
}

HealthResponse HealthService::get_health() const {
    HealthResponse response;
    response.is_leader = is_leader_.load(std::memory_order_relaxed);
    response.connected_peers = connected_peers_.load(std::memory_order_relaxed);
    response.queue_depth = queue_depth_.load(std::memory_order_relaxed);
    
    if (!is_ready()) {
        response.status = HealthStatus::UNHEALTHY;
        response.message = "Service not ready";
    } else if (connected_peers_.load() == 0 && !is_leader_.load()) {
        response.status = HealthStatus::DEGRADED;
        response.message = "No peers connected";
    } else {
        response.status = HealthStatus::HEALTHY;
        response.message = "OK";
    }
    
    return response;
}

void HealthService::set_ready(bool ready) {
    ready_.store(ready, std::memory_order_relaxed);
}

void HealthService::set_leader(bool is_leader) {
    is_leader_.store(is_leader, std::memory_order_relaxed);
}

void HealthService::set_connected_peers(size_t count) {
    connected_peers_.store(count, std::memory_order_relaxed);
}

void HealthService::set_queue_depth(size_t depth) {
    queue_depth_.store(depth, std::memory_order_relaxed);
}

} // namespace eventstream::microservice
