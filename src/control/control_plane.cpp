#include "control/control_plane.hpp"
#include <spdlog/spdlog.h>

static constexpr size_t QUEUE_DEPTH_THRESHOLD = 10'000;
static constexpr uint64_t AVG_LATENCY_LIMIT_NS = 200'000'000;  // 200ms in nanoseconds
static constexpr uint64_t ERROR_RATE_LIMIT = 5;  // 5% error rate

ControlDecision ControlPlane::MakeDecision(const MetricSnapshot& snapshot) {
    // Check queue depth first (most critical)
    if (snapshot.current_queue_depth > QUEUE_DEPTH_THRESHOLD) {
        spdlog::warn("Control decision: OVERLOADED - queue depth {}", snapshot.current_queue_depth);
        return {
            FailureState::Overloaded,
            ControlAction::PauseTransactions,
            "Queue overload detected"
        };
    }

    // Check average latency
    uint64_t avg_latency_ns = snapshot.get_avg_latency_ns();
    if (avg_latency_ns > AVG_LATENCY_LIMIT_NS) {
        spdlog::warn("Control decision: LATENCY_SPIKE - avg latency {} ns", avg_latency_ns);
        return {
            FailureState::LatencySpike,
            ControlAction::DropBatchEvents,
            "Latency spike detected"
        };
    }

    // Check error rate
    uint64_t total_events = snapshot.total_events_processed + snapshot.total_events_dropped;
    uint64_t error_rate = total_events > 0 ? (snapshot.total_events_errors * 100) / total_events : 0;
    if (error_rate > ERROR_RATE_LIMIT) {
        spdlog::warn("Control decision: ERROR_BURST - error rate {}%", error_rate);
        return {
            FailureState::ErrorBurst,
            ControlAction::PushDLQ,
            "Error burst detected"
        };
    }

    // All healthy
    spdlog::debug("Control decision: HEALTHY");
    return {
        FailureState::Healthy,
        ControlAction::ResumeTransactions,
        "System healthy"
    };
}