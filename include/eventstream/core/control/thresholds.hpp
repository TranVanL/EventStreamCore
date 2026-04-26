#pragma once

#include <cstdint>
#include <cstddef>

namespace EventStream {

struct ControlThresholds {
    std::size_t max_queue_depth;
    double max_drop_rate;
    std::uint64_t max_latency_ms;
    std::size_t min_events_for_evaluation;
    double recovery_factor;
};

} // namespace EventStream