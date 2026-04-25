#pragma once

#include <atomic>
#include <string>

namespace eventstream::microservice {

enum class HealthStatus { HEALTHY, DEGRADED, UNHEALTHY };

struct HealthResponse {
    HealthStatus status = HealthStatus::HEALTHY;
    std::string message;
    bool is_leader = false;
    size_t connected_peers = 0;
    size_t queue_depth = 0;
};

class HealthService {
public:
    HealthService();

    bool is_alive() const;
    bool is_ready() const;
    HealthResponse get_health() const;

    void set_ready(bool ready);
    void set_leader(bool is_leader);
    void set_connected_peers(size_t count);
    void set_queue_depth(size_t depth);

private:
    std::atomic<bool> ready_{false};
    std::atomic<bool> is_leader_{false};
    std::atomic<size_t> connected_peers_{0};
    std::atomic<size_t> queue_depth_{0};
};

} // namespace eventstream::microservice
