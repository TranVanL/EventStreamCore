#include "control/ControlPlane.hpp"
#include <spdlog/spdlog.h>

using namespace EventStream;

ControlPlane::ControlPlane() {
    // No logging on init - reduce noise
}

// ============================================================================
// Simplified Decision Logic
// ============================================================================

EventControlDecision ControlPlane::evaluateMetrics(
    uint64_t queue_depth,
    uint64_t total_processed,
    uint64_t total_dropped,
    uint64_t latency_ms
) {
    // Calculate drop rate
    double drop_rate = total_processed > 0 
        ? (total_dropped * 100.0) / total_processed 
        : 0;
    
    // Simple check: if queue depth OR drop rate exceeds threshold -> UNHEALTHY
    if (queue_depth > thresholds_.max_queue_depth || 
        drop_rate > thresholds_.max_drop_rate) {
        return EventControlDecision(
            ControlAction::PAUSE_PROCESSOR,
            FailureState::CRITICAL,
            "Metrics exceed thresholds"
        );
    }
    
    // Otherwise system is healthy
    return EventControlDecision(
        ControlAction::RESUME,
        FailureState::HEALTHY,
        "Metrics healthy"
    );
}


void ControlPlane::executeDecision(
    const EventControlDecision& decision,
    PipelineStateManager& state_manager
) {
    // Minimal state mapping - no verbose logging
    PipelineState newState = (decision.action == ControlAction::RESUME) 
        ? PipelineState::RUNNING 
        : PipelineState::PAUSED;
    
    state_manager.setState(newState);
}

