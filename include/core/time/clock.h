// ============================================================================
// HIGH-PRECISION MONOTONIC CLOCK
// ============================================================================
// Day 32: Fixed clock abstraction using steady_clock
// 
// Problem: high_resolution_clock is not monotonic
// - Subject to OS scheduling
// - Affected by NTP time adjustments
// - Results in inflated latency measurements
//
// Solution: Use std::chrono::steady_clock
// - Monotonic (always moves forward)
// - Not affected by system time adjustments
// - Accurate for latency measurement
// ============================================================================

#pragma once

#include <chrono>
#include <cstdint>

namespace EventStream {

class Clock {
public:
    // Get current time in nanoseconds (monotonic, steady)
    static inline uint64_t now_ns() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();
    }
    
    // Get current time in microseconds
    static inline uint64_t now_us() {
        return now_ns() / 1000;
    }
    
    // Get current time in milliseconds
    static inline uint64_t now_ms() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }
};

} // namespace EventStream
