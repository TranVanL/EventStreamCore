#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>
#include <eventstream/core/events/dispatcher.hpp>

/**
 * @class IngestServer
 * @brief Abstract base for network ingest servers (TCP, UDP, etc.).
 */
class IngestServer {
public:
    explicit IngestServer(Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
    virtual ~IngestServer() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

protected:
    virtual void acceptConnections() = 0;

    Dispatcher& dispatcher_;
};


