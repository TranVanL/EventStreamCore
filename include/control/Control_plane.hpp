#pragma once 
#include "metrics/metrics.hpp" 
#include <string>

enum class FailureState {
    Healthy = 0,
    Overloaded = 1,
    LatencySpike = 2,
    ErrorBurst = 3
};

enum class ControlAction {
    NoAction = 0,
    PauseTransactions = 1,
    ResumeTransactions = 2,
    DropBatchEvents = 3,
    PushDLQ = 4
};

class ControlDecision {
public:
    FailureState state;
    ControlAction action;
    std::string reason;

    ControlDecision(FailureState s, ControlAction a, const std::string& r)
        : state(s), action(a), reason(r) {}
};

class ControlPlane {
public:
    ControlPlane() = default;
    ~ControlPlane() = default;

    ControlDecision MakeDecision(const MetricSnapshot& snapshot){
        // Check queue depth first (most critical)
        if (snapshot.current_queue_depth > 10000) {
            return ControlDecision(
                FailureState::Overloaded,
                ControlAction::PauseTransactions,
                "Queue overload detected"
            );
        }

        // Check average latency
        uint64_t avg_latency_ns = snapshot.get_avg_latency_ns();
        if (avg_latency_ns > 200'000'000) {  // 200ms
            return ControlDecision(
                FailureState::LatencySpike,
                ControlAction::DropBatchEvents,
                "Latency spike detected"
            );
        }

        // Check error rate
        uint64_t total_events = snapshot.total_events_processed + snapshot.total_events_dropped;
        uint64_t error_rate = total_events > 0 ? (snapshot.total_events_errors * 100) / total_events : 0;
        if (error_rate > 5) {  // 5%
            return ControlDecision(
                FailureState::ErrorBurst,
                ControlAction::PushDLQ,
                "Error burst detected"
            );
        }

        // All healthy
        return ControlDecision(
            FailureState::Healthy,
            ControlAction::ResumeTransactions,
            "System healthy"
        );
    };
};