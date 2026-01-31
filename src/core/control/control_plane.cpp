#include <eventstream/core/control/control_plane.hpp>
#include <spdlog/spdlog.h>

using namespace EventStream;

ControlPlane::ControlPlane() {
    spdlog::debug("[ControlPlane] Initialized with thresholds: max_queue={}, max_drop_rate={}%",
                  thresholds_.max_queue_depth, thresholds_.max_drop_rate);
}

// ============================================================================
// Multi-Level Decision Logic
// ============================================================================
// Decision tree based on system health metrics:
//
// Level 1 (HEALTHY):     drop_rate < 1%   AND queue < 50% of max  -> RESUME
// Level 2 (ELEVATED):    drop_rate < 2%   AND queue < 75% of max  -> RESUME (log warning)
// Level 3 (DEGRADED):    drop_rate < 5%   AND queue < 100% of max -> DROP_BATCH
// Level 4 (CRITICAL):    drop_rate >= 5%  OR  queue >= 100% max   -> PAUSE
// Level 5 (EMERGENCY):   drop_rate >= 10% OR  queue > 150% max    -> EMERGENCY (DLQ)
// ============================================================================

EventControlDecision ControlPlane::evaluateMetrics(
    uint64_t queue_depth,
    uint64_t total_processed,
    uint64_t total_dropped,
    uint64_t latency_ms
) {
    // Calculate drop rate as percentage of total events
    uint64_t total_events = total_processed + total_dropped;
    double drop_rate = total_events > 0 
        ? (total_dropped * 100.0) / total_events 
        : 0.0;
    
    // ========================================================================
    // Early exit: Not enough events to make a meaningful decision
    // Prevents false positives during startup (e.g., 1 drop out of 10 = 10%!)
    // ========================================================================
    if (total_events < thresholds_.min_events_for_evaluation) {
        // During warmup, only react to queue depth issues
        double queue_util = (thresholds_.max_queue_depth > 0)
            ? (queue_depth * 100.0) / thresholds_.max_queue_depth
            : 0.0;
        
        if (queue_util >= 100.0) {
            spdlog::warn("[ControlPlane] WARMUP but queue full: queue_util={:.1f}%", queue_util);
            return EventControlDecision(
                ControlAction::PAUSE_PROCESSOR,
                FailureState::CRITICAL,
                "Warmup: Queue full"
            );
        }
        return EventControlDecision(
            ControlAction::RESUME,
            FailureState::HEALTHY,
            "Warmup: Collecting baseline metrics"
        );
    }
    
    // Queue utilization as percentage of max
    double queue_util = (thresholds_.max_queue_depth > 0)
        ? (queue_depth * 100.0) / thresholds_.max_queue_depth
        : 0.0;
    
    // ========================================================================
    // Level 5: EMERGENCY (extreme overload)
    // ========================================================================
    if (drop_rate >= 10.0 || queue_util > 150.0) {
        previous_state_ = FailureState::CRITICAL;
        spdlog::error("[ControlPlane] EMERGENCY: drop_rate={:.1f}%, queue_util={:.1f}%",
                      drop_rate, queue_util);
        return EventControlDecision(
            ControlAction::PUSH_DLQ,
            FailureState::CRITICAL,
            "Emergency: Extreme overload detected, pushing to DLQ"
        );
    }
    
    // ========================================================================
    // Level 4: CRITICAL (threshold exceeded)
    // ========================================================================
    if (drop_rate >= thresholds_.max_drop_rate || queue_util >= 100.0) {
        previous_state_ = FailureState::CRITICAL;
        spdlog::warn("[ControlPlane] CRITICAL: drop_rate={:.1f}%, queue_util={:.1f}%",
                     drop_rate, queue_util);
        return EventControlDecision(
            ControlAction::PAUSE_PROCESSOR,
            FailureState::CRITICAL,
            "Critical: Metrics exceed thresholds"
        );
    }
    
    // ========================================================================
    // Level 3: DEGRADED (approaching threshold)
    // ========================================================================
    if (drop_rate >= thresholds_.max_drop_rate * 0.5 || queue_util >= 75.0) {
        // Only escalate if we were already degraded (hysteresis)
        if (previous_state_ != FailureState::HEALTHY) {
            previous_state_ = FailureState::DEGRADED;
            spdlog::warn("[ControlPlane] DEGRADED: drop_rate={:.1f}%, queue_util={:.1f}% - dropping batch",
                         drop_rate, queue_util);
            return EventControlDecision(
                ControlAction::DROP_BATCH,
                FailureState::DEGRADED,
                "Degraded: Proactive batch drop to reduce load"
            );
        }
        // First time crossing threshold - just warn
        spdlog::info("[ControlPlane] ELEVATED: drop_rate={:.1f}%, queue_util={:.1f}%",
                     drop_rate, queue_util);
    }
    
    // ========================================================================
    // Level 1-2: HEALTHY (normal operation)
    // ========================================================================
    previous_state_ = FailureState::HEALTHY;
    return EventControlDecision(
        ControlAction::RESUME,
        FailureState::HEALTHY,
        "Healthy: Metrics within normal range"
    );
}


void ControlPlane::executeDecision(
    const EventControlDecision& decision,
    PipelineStateManager& state_manager
) {
    // Map ControlAction to PipelineState
    // This provides more granular control than simple RUNNING/PAUSED
    PipelineState newState;
    
    switch (decision.action) {
        case ControlAction::RESUME:
        case ControlAction::NONE:
            newState = PipelineState::RUNNING;
            break;
        case ControlAction::DRAIN:
            newState = PipelineState::DRAINING;
            break;
        case ControlAction::DROP_BATCH:
            newState = PipelineState::DROPPING;
            break;
        case ControlAction::PUSH_DLQ:
            newState = PipelineState::EMERGENCY;
            break;
        case ControlAction::PAUSE_PROCESSOR:
        default:
            newState = PipelineState::PAUSED;
            break;
    }
    
    state_manager.setState(newState);
}

