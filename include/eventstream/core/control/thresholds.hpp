#pragma once

#include <cstdint>

namespace EventStream {

/**
 * @struct ControlThresholds
 * @brief Thresholds for control plane decision making
 * 
 * These thresholds define the boundaries between health levels:
 * - HEALTHY:   drop_rate < max_drop_rate/2, queue < max_queue_depth * 0.5
 * - ELEVATED:  drop_rate < max_drop_rate,   queue < max_queue_depth * 0.75
 * - DEGRADED:  drop_rate < max_drop_rate*2, queue < max_queue_depth
 * - CRITICAL:  drop_rate >= max_drop_rate,  queue >= max_queue_depth
 * - EMERGENCY: drop_rate >= 10%,            queue > max_queue_depth * 1.5
 */
struct ControlThresholds {
    // Maximum acceptable queue depth before action is taken
    uint64_t max_queue_depth = 5000;
    
    // Maximum acceptable drop rate (percentage) before action is taken
    double max_drop_rate = 2.0;
    
    // Maximum acceptable latency (milliseconds) - for future use
    uint64_t max_latency_ms = 100;
    
    // Minimum events processed before evaluating drop rate
    // (avoids false positives during startup)
    uint64_t min_events_for_evaluation = 1000;
    
    // Hysteresis factor - how much below threshold to recover
    // e.g., 0.8 means recover when metrics drop to 80% of threshold
    double recovery_factor = 0.8;
};

} 
