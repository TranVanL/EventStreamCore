#pragma once

#include <cstdint>

namespace EventStream {

struct ControlThresholds {
    // Single queue depth threshold
    uint64_t max_queue_depth = 5000;
    
    // Single drop rate threshold (percentage)
    double max_drop_rate = 2.0;
};

} 
