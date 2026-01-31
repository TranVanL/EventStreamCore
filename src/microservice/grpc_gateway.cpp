/**
 * @file grpc_gateway.cpp
 * @brief gRPC Gateway implementation (placeholder)
 * 
 * TODO: Implement with actual gRPC when grpc library is added
 */

#include <eventstream/microservice/grpc_gateway.hpp>
#include <spdlog/spdlog.h>

namespace eventstream::microservice {

struct GrpcGateway::Impl {
    GrpcConfig config;
    bool running = false;
};

GrpcGateway::GrpcGateway(const GrpcConfig& config) 
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
}

GrpcGateway::~GrpcGateway() {
    stop();
}

bool GrpcGateway::start() {
    if (impl_->running) {
        return true;
    }
    
    spdlog::info("Starting gRPC Gateway on {}:{}", 
                 impl_->config.host, impl_->config.port);
    
    // TODO: Implement actual gRPC server start
    impl_->running = true;
    return true;
}

void GrpcGateway::stop() {
    if (!impl_->running) {
        return;
    }
    
    spdlog::info("Stopping gRPC Gateway");
    impl_->running = false;
}

bool GrpcGateway::is_running() const {
    return impl_->running;
}

std::string GrpcGateway::get_address() const {
    return impl_->config.host + ":" + std::to_string(impl_->config.port);
}

} // namespace eventstream::microservice
