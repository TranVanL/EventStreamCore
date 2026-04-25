#pragma once

#include <string>
#include <memory>

namespace eventstream::microservice {

struct GrpcConfig {
    std::string host = "0.0.0.0";
    int port = 50051;
    int max_threads = 4;
};

class GrpcGateway {
public:
    explicit GrpcGateway(const GrpcConfig& config = {});
    ~GrpcGateway();

    bool start();
    void stop();
    bool is_running() const;
    std::string get_address() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eventstream::microservice
