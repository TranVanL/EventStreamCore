#pragma once 
#include <atomic>
#include <cstdint>

struct Metrics
{
    std::atomic<uint64_t> total_events_processed{0};
    std::atomic<uint64_t> total_events_dropped{0};
    std::atomic<uint64_t> total_events_alerted{0};
    std::atomic<uint64_t> total_events_errors{0};
    std::atomic<uint64_t> total_events_skipped{0};  // For idempotent/duplicate skips
};
