/**
 * @file grpc_gateway.hpp
 * @brief gRPC Gateway for EventStreamCore
 * 
 * Provides gRPC interface for external clients (Python, Go, Java, etc.)
 * to interact with the EventStreamCore engine.
 * 
 * @author EventStreamCore Team
 * @version 0.1
 */

#pragma once

#include <string>
#include <memory>
#include <functional>
#include <cstdint>

namespace eventstream::microservice {

/**
 * @brief gRPC Gateway configuration
 */
struct GrpcConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 9200;
    bool enable_tls = false;
    std::string cert_path;
    std::string key_path;
    size_t max_message_size = 4 * 1024 * 1024;  // 4MB
    size_t thread_pool_size = 4;
};

/**
 * @brief gRPC Gateway for EventStreamCore
 * 
 * Exposes EventStreamCore functionality via gRPC for:
 * - Publishing events
 * - Subscribing to topics
 * - Health checks
 * - Metrics collection
 */
class GrpcGateway {
public:
    explicit GrpcGateway(const GrpcConfig& config);
    ~GrpcGateway();
    
    // Non-copyable
    GrpcGateway(const GrpcGateway&) = delete;
    GrpcGateway& operator=(const GrpcGateway&) = delete;
    
    /**
     * @brief Start the gRPC server
     * @return true if started successfully
     */
    bool start();
    
    /**
     * @brief Stop the gRPC server gracefully
     */
    void stop();
    
    /**
     * @brief Check if server is running
     */
    bool is_running() const;
    
    /**
     * @brief Get server address
     */
    std::string get_address() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eventstream::microservice
