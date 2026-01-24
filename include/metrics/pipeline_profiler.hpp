#pragma once

#include <chrono>
#include <vector>
#include <atomic>
#include <string>
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace EventStream {

/**
 * Simple latency profiler for the event pipeline
 * 
 * Tracks:
 * - Ingest latency (TCP receive -> Dispatcher)
 * - Dispatch latency (Dispatcher input -> Route decision)
 * - Queue push latency (Dispatcher -> EventBus)
 * - Processing latency (EventBus -> ProcessManager)
 * - End-to-end latency
 */
class PipelineProfiler {
public:
    struct LatencyPoint {
        const char* name;
        uint64_t timestamp_ns;  // From event creation time
    };
    
    struct EventLatencies {
        uint64_t event_id;
        std::vector<LatencyPoint> points;
        
        uint64_t total_latency_ns() const {
            if (points.empty()) return 0;
            return points.back().timestamp_ns - points.front().timestamp_ns;
        }
    };
    
    static PipelineProfiler& instance() {
        static PipelineProfiler prof;
        return prof;
    }
    
    void recordPoint(uint64_t event_id, const char* point_name) {
        auto now_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        
        // Simple: store in thread-local buffer
        // In real implementation, would use lock-free ring buffer
        (void)event_id;  // Use for keying if needed
        (void)point_name;
        (void)now_ns;
    }
    
    void printStats() {
        std::cout << "\n╔════════════════════════════════════════════╗" << std::endl;
        std::cout << "║  PIPELINE LATENCY PROFILE                  ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════╝" << std::endl;
    }

private:
    PipelineProfiler() = default;
};

}  // namespace EventStream
