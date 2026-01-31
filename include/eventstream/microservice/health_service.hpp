/**
 * @file health_service.hpp
 * @brief Health check service for EventStreamCore
 * 
 * Provides health endpoints for Kubernetes liveness/readiness probes
 * and monitoring systems.
 * 
 * @author EventStreamCore Team
 * @version 0.1
 */

#pragma once

#include <string>
#include <atomic>

namespace eventstream::microservice {

/**
 * @brief Health status enumeration
 */
enum class HealthStatus {
    UNKNOWN,
    HEALTHY,
    DEGRADED,
    UNHEALTHY
};

/**
 * @brief Health check response
 */
struct HealthResponse {
    HealthStatus status;
    std::string message;
    bool is_leader;
    size_t connected_peers;
    size_t queue_depth;
};

/**
 * @brief Health service for EventStreamCore
 */
class HealthService {
public:
    HealthService();
    ~HealthService() = default;
    
    /**
     * @brief Liveness check - is the process alive?
     */
    bool is_alive() const;
    
    /**
     * @brief Readiness check - is the service ready to accept traffic?
     */
    bool is_ready() const;
    
    /**
     * @brief Get detailed health status
     */
    HealthResponse get_health() const;
    
    /**
     * @brief Set ready state
     */
    void set_ready(bool ready);
    
    /**
     * @brief Set leader state
     */
    void set_leader(bool is_leader);
    
    /**
     * @brief Update connected peers count
     */
    void set_connected_peers(size_t count);
    
    /**
     * @brief Update queue depth
     */
    void set_queue_depth(size_t depth);

private:
    std::atomic<bool> ready_{false};
    std::atomic<bool> is_leader_{false};
    std::atomic<size_t> connected_peers_{0};
    std::atomic<size_t> queue_depth_{0};
};

} // namespace eventstream::microservice
