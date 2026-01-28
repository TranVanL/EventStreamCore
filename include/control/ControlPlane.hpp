#pragma once

#include <cstdint>
#include <string>
#include "control/PipelineState.hpp"
#include "control/ControlThresholds.hpp"
#include "admin/ControlDecision.hpp"

namespace EventStream {

class ControlPlane {
public:
    ControlPlane();
    ~ControlPlane() = default;
    EventControlDecision evaluateMetrics(
        uint64_t queue_depth,
        uint64_t total_processed,
        uint64_t total_dropped,
        uint64_t latency_ms
    );

    void executeDecision(
        const EventControlDecision& decision,
        PipelineStateManager& state_manager
    );

    const ControlThresholds& getThresholds() const { return thresholds_; }
    void setThresholds(const ControlThresholds& t) { thresholds_ = t; }

private:
    ControlThresholds thresholds_;
};

} // namespace EventStream 
