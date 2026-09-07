#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>
#include <eventstream/core/events/dispatcher.hpp>
#include <eventstream/rt/rt_policy.hpp>
#include <eventstream/rt/rt_thread.hpp>

/**
 * @class IngestServer
 * @brief Abstract base for network ingest servers (TCP, UDP, etc.).
 */

 // Build a base class for IngestServer , Abstraction and can open different type of server (TCP, UDP, etc.) to receive event from network and push to dispatcher
class IngestServer {
public:
    explicit IngestServer(Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
    virtual ~IngestServer() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

protected:
    virtual void acceptConnections() = 0;

    void applyIngestPolicy(std::thread& thread, const char* workerName) const {
        try {
            if (eventstream::rt::RtThread::apply(thread, ingestPolicy_)) {
                spdlog::info("[RT] {} policy applied: {}",
                             workerName, eventstream::rt::RtThread::describe(ingestPolicy_));
            } else {
                spdlog::warn("[RT] {} policy was not fully applied; "
                             "worker continues with best-effort scheduling/affinity",
                             workerName);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[RT] Failed to apply {} policy: {}; "
                         "worker continues with best-effort scheduling/affinity",
                         workerName, e.what());
        }
    }

    Dispatcher& dispatcher_;
    eventstream::rt::RtPolicy ingestPolicy_ =
        eventstream::rt::RtPolicyBuilder().fifo().priority(60).cpus({1}).build();
};


