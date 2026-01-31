#pragma once
#include <atomic>
#include <cstdint>
#include <spdlog/spdlog.h>

/**
 * Pipeline State Machine - Common language between Admin (decision maker) and Workers (executors)
 * 
 * AdminLoop is the "brain" - sole authority to change state
 * Workers are the "body" - only read state, never modify
 */
enum class PipelineState : uint8_t {
    RUNNING = 0,      // Normal operation - ingest and process as usual
    PAUSED = 1,       // Workers stop consuming from queue, backlog accumulates
    DRAINING = 2,     // Stop new ingest, workers drain remaining backlog
    DROPPING = 3,     // Controlled batch event dropping
    EMERGENCY = 4     // Push all failed events to DLQ
};

/**
 * Thread-safe pipeline state manager (read-heavy workload)
 */
class PipelineStateManager {
public:
    PipelineStateManager();
    ~PipelineStateManager() = default;

    /**
     * Only AdminLoop should call this
     * Others: no authority to change state
     */
    void setState(PipelineState newState);

    /**
     * Worker/Dispatcher calls to read current state
     * Non-blocking, memory_order_acquire
     */
    PipelineState getState() const;

    /**
     * Worker checks before consuming/processing event
     */
    bool isRunning() const {
        return getState() == PipelineState::RUNNING;
    }

    bool isPaused() const {
        return getState() == PipelineState::PAUSED;
    }

    bool isDraining() const {
        return getState() == PipelineState::DRAINING;
    }

    bool isDropping() const {
        return getState() == PipelineState::DROPPING;
    }

    bool isEmergency() const {
        return getState() == PipelineState::EMERGENCY;
    }

    /**
     * String representation for logging
     */
    static const char* toString(PipelineState state);

private:
    std::atomic<PipelineState> state_{PipelineState::RUNNING};
};
