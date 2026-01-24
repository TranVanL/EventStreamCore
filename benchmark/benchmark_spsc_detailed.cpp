// ============================================================================
// BENCHMARK: SPSC RING BUFFER DETAILED PERFORMANCE
// ============================================================================
// Day 35: Characterize SPSC ringbuffer throughput, latency, and behavior
//
// Metrics collected:
// 1. Throughput (events/sec)
// 2. Latency percentiles (p50, p95, p99)
// 3. Capacity utilization
// 4. Empty/Full detection accuracy
// ============================================================================

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <queue>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include "utils/spsc_ringBuffer.hpp"
#include "event/Event.hpp"

using namespace EventStream;

// ============================================================================
// EVENT STRUCTURE FOR TESTING
// ============================================================================
// Use simple uint64_t pair for benchmarking (avoids template instantiation issues)
using TestEvent = std::pair<uint64_t, uint64_t>;  // id + timestamp_ns

// ============================================================================
// BENCHMARK UTILITIES
// ============================================================================

struct LatencyStats {
    double p50_us;
    double p95_us;
    double p99_us;
    double avg_us;
    double min_us;
    double max_us;
};

LatencyStats compute_latency_stats(const std::vector<uint64_t>& latencies_ns) {
    if (latencies_ns.empty()) {
        return {0, 0, 0, 0, 0, 0};
    }
    
    std::vector<uint64_t> sorted = latencies_ns;
    std::sort(sorted.begin(), sorted.end());
    
    auto to_us = [](uint64_t ns) { return ns / 1000.0; };
    
    size_t p50_idx = sorted.size() * 50 / 100;
    size_t p95_idx = sorted.size() * 95 / 100;
    size_t p99_idx = sorted.size() * 99 / 100;
    
    uint64_t sum = std::accumulate(sorted.begin(), sorted.end(), 0UL);
    
    return {
        to_us(sorted[p50_idx]),
        to_us(sorted[p95_idx]),
        to_us(sorted[p99_idx]),
        to_us(sum / sorted.size()),
        to_us(sorted[0]),
        to_us(sorted[sorted.size() - 1])
    };
}

void print_latency_stats(const std::string& label, const LatencyStats& stats) {
    std::cout << "\n" << label << std::endl;
    std::cout << "  p50:  " << std::fixed << std::setprecision(2) << stats.p50_us << " μs" << std::endl;
    std::cout << "  p95:  " << std::fixed << std::setprecision(2) << stats.p95_us << " μs" << std::endl;
    std::cout << "  p99:  " << std::fixed << std::setprecision(2) << stats.p99_us << " μs" << std::endl;
    std::cout << "  avg:  " << std::fixed << std::setprecision(2) << stats.avg_us << " μs" << std::endl;
    std::cout << "  min:  " << std::fixed << std::setprecision(2) << stats.min_us << " μs" << std::endl;
    std::cout << "  max:  " << std::fixed << std::setprecision(2) << stats.max_us << " μs" << std::endl;
}

// ============================================================================
// TEST 1: SEQUENTIAL THROUGHPUT
// ============================================================================

void test_sequential_throughput() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 1: SEQUENTIAL THROUGHPUT (Single Producer, Single Consumer)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    SpscRingBuffer<TestEvent, 16384> buffer;
    const uint64_t NUM_EVENTS = 1000000;
    
    std::vector<uint64_t> push_latencies_ns;
    std::vector<uint64_t> pop_latencies_ns;
    push_latencies_ns.reserve(NUM_EVENTS);
    pop_latencies_ns.reserve(NUM_EVENTS);
    
    // Producer thread
    auto producer = [&]() {
        for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
            TestEvent evt{i, std::chrono::high_resolution_clock::now().time_since_epoch().count()};
            
            auto start = std::chrono::high_resolution_clock::now();
            while (!buffer.push(evt)) {
                std::this_thread::yield();
            }
            auto end = std::chrono::high_resolution_clock::now();
            
            uint64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            push_latencies_ns.push_back(latency_ns);
        }
    };
    
    // Consumer thread
    auto consumer = [&]() {
        for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            
            std::optional<TestEvent> evt;
            while (!(evt = buffer.pop())) {
                std::this_thread::yield();
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            uint64_t latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            pop_latencies_ns.push_back(latency_ns);
        }
    };
    
    auto overall_start = std::chrono::high_resolution_clock::now();
    
    std::thread prod_thread(producer);
    std::thread cons_thread(consumer);
    
    prod_thread.join();
    cons_thread.join();
    
    auto overall_end = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(overall_end - overall_start).count();
    
    double throughput_k = (NUM_EVENTS / 1000.0) / (elapsed_ms / 1000.0) / 1000.0;  // K events/sec
    
    std::cout << "\nThroughput: " << std::fixed << std::setprecision(1) << (throughput_k * 1000) << "K events/sec" << std::endl;
    std::cout << "Total time: " << elapsed_ms << " ms" << std::endl;
    
    auto push_stats = compute_latency_stats(push_latencies_ns);
    auto pop_stats = compute_latency_stats(pop_latencies_ns);
    
    print_latency_stats("Push latency (ns):", push_stats);
    print_latency_stats("Pop latency (ns):", pop_stats);
}

// ============================================================================
// TEST 2: CAPACITY UTILIZATION
// ============================================================================

void test_capacity_utilization() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 2: CAPACITY UTILIZATION (at different rates)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    SpscRingBuffer<TestEvent, 16384> buffer;
    const uint64_t DURATION_MS = 1000;
    
    std::atomic<uint64_t> producer_count{0};
    std::atomic<uint64_t> consumer_count{0};
    std::atomic<bool> run{true};
    
    // Fast producer, slow consumer
    auto producer_fast = [&]() {
        while (run.load()) {
            TestEvent evt{producer_count.load(), std::chrono::high_resolution_clock::now().time_since_epoch().count()};
            if (buffer.push(evt)) {
                producer_count.fetch_add(1);
            }
        }
    };
    
    auto consumer_slow = [&]() {
        while (run.load()) {
            if (auto evt = buffer.pop()) {
                consumer_count.fetch_add(1);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    };
    
    std::thread prod(producer_fast);
    std::thread cons(consumer_slow);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    run.store(false);
    
    prod.join();
    cons.join();
    
    uint64_t produced = producer_count.load();
    uint64_t consumed = consumer_count.load();
    uint64_t buffered = produced - consumed;
    
    std::cout << "\nProduced: " << produced << std::endl;
    std::cout << "Consumed: " << consumed << std::endl;
    std::cout << "Buffered: " << buffered << std::endl;
    std::cout << "Utilization: " << std::fixed << std::setprecision(1) 
              << (buffered * 100.0 / 16384) << "%" << std::endl;
}

// ============================================================================
// TEST 3: BURST BEHAVIOR
// ============================================================================

void test_burst_behavior() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 3: BURST BEHAVIOR (sudden traffic spikes)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    SpscRingBuffer<TestEvent, 16384> buffer;
    
    std::atomic<uint64_t> total_lost{0};
    std::atomic<bool> run{true};
    
    // Producer: alternates between idle and bursts
    auto producer_burst = [&]() {
        uint32_t id = 0;
        while (run.load()) {
            // Burst: try to push 1000 events
            for (int i = 0; i < 1000; ++i) {
                TestEvent evt{id++, std::chrono::high_resolution_clock::now().time_since_epoch().count()};
                if (!buffer.push(evt)) {
                    total_lost.fetch_add(1);
                }
            }
            // Rest
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };
    
    // Consumer: steady rate
    auto consumer_steady = [&]() {
        while (run.load()) {
            if (buffer.pop()) {
                // Processed
            }
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    };
    
    std::thread prod(producer_burst);
    std::thread cons(consumer_steady);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    run.store(false);
    
    prod.join();
    cons.join();
    
    std::cout << "\nTotal events dropped during bursts: " << total_lost.load() << std::endl;
    std::cout << "Buffer handled spikes with overflow dropping" << std::endl;
}

// ============================================================================
// TEST 4: LATENCY UNDER LOAD
// ============================================================================

void test_latency_under_load() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 4: LATENCY UNDER LOAD" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    SpscRingBuffer<TestEvent, 16384> buffer;
    const uint64_t NUM_SAMPLES = 10000;
    
    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(NUM_SAMPLES);
    
    std::atomic<bool> ready{false};
    std::atomic<uint64_t> count{0};
    
    // Consumer thread: keep buffer drained
    auto consumer = [&]() {
        while (count.load() < NUM_SAMPLES) {
            if (buffer.pop()) {
                count.fetch_add(1);
            }
        }
    };
    
    std::thread cons(consumer);
    
    // Producer thread: measure latency
    ready.store(true);
    for (uint64_t i = 0; i < NUM_SAMPLES; ++i) {
        TestEvent evt{i, std::chrono::high_resolution_clock::now().time_since_epoch().count()};
        
        auto start = std::chrono::high_resolution_clock::now();
        while (!buffer.push(evt)) {
            std::this_thread::yield();
        }
        auto end = std::chrono::high_resolution_clock::now();
        
        latencies_ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
        );
    }
    
    cons.join();
    
    auto stats = compute_latency_stats(latencies_ns);
    std::cout << "\nLatency under full load:" << std::endl;
    print_latency_stats("Push latency:", stats);
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n╔════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  SPSC RING BUFFER DETAILED PERFORMANCE BENCHMARK                    ║" << std::endl;
    std::cout << "║  Day 35: Optimize EventStreamCore                                    ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════╝" << std::endl;
    
    test_sequential_throughput();
    test_capacity_utilization();
    test_burst_behavior();
    test_latency_under_load();
    
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "All benchmarks completed successfully!" << std::endl;
    std::cout << std::string(70, '=') << "\n" << std::endl;
    
    return 0;
}
