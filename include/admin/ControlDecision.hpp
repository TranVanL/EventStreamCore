#pragma once

#include <cstdint>
#include <string>

namespace EventStream {

enum class ControlAction : uint8_t {
    NONE = 0,                  // No action needed - system healthy
    PAUSE_PROCESSOR = 1,       // Stop processor from consuming events
    DROP_BATCH = 2,            // Drop N events to DLQ to reduce load
    DRAIN = 3,                 // Finish current work, then pause
    PUSH_DLQ = 4,              // Push failed events to DLQ
    RESUME = 5                 // Resume normal operation
};

/**
 * @enum FailureState
 * @brief System health classification
 */
enum class FailureState : uint8_t {
    HEALTHY = 0,      // All metrics normal
    DEGRADED = 1,     // Some metrics elevated
    CRITICAL = 2      // Metrics exceed critical thresholds
};

/**
 * @struct EventControlDecision
 * @brief Formal decision object for control plane
 */
struct EventControlDecision {
    ControlAction action;
    FailureState reason;
    std::string details;
    uint64_t timestamp_ms{0};
    
    EventControlDecision() 
        : action(ControlAction::NONE), 
          reason(FailureState::HEALTHY),
          details("") {}
    
    EventControlDecision(ControlAction a, FailureState r, const std::string& d)
        : action(a), reason(r), details(d) {}
    
    static const char* actionString(ControlAction a) {
        switch (a) {
            case ControlAction::NONE: return "NONE";
            case ControlAction::PAUSE_PROCESSOR: return "PAUSE_PROCESSOR";
            case ControlAction::DROP_BATCH: return "DROP_BATCH";
            case ControlAction::DRAIN: return "DRAIN";
            case ControlAction::PUSH_DLQ: return "PUSH_DLQ";
            case ControlAction::RESUME: return "RESUME";
            default: return "UNKNOWN";
        }
    }
    
    static const char* failureStateString(FailureState s) {
        switch (s) {
            case FailureState::HEALTHY: return "HEALTHY";
            case FailureState::DEGRADED: return "DEGRADED";
            case FailureState::CRITICAL: return "CRITICAL";
            default: return "UNKNOWN";
        }
    }
};

} // namespace EventStream
