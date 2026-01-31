// ============================================================================
// EVENTSTREAM CORE - COMPREHENSIVE BENCHMARK SUITE
// ============================================================================
// Run all performance benchmarks and generate a consolidated report
//
// Components tested:
// 1. SpscRingBuffer - Lock-free Single Producer Single Consumer
// 2. MpscQueue - Lock-free Multi Producer Single Consumer (Vyukov)
// 3. LockFreeDeduplicator - Atomic hash map for idempotency
// 4. EventBusMulti - Multi-queue event router
// 5. EventPool - Zero-allocation object reuse
//
// Output: Summary of all throughput and latency metrics
// ============================================================================

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <sstream>

#include <eventstream/core/queues/spsc_ring_buffer.hpp>
#include <eventstream/core/queues/mpsc_queue.hpp>
#include <eventstream/core/queues/lock_free_dedup.hpp>
#include <eventstream/core/events/event.hpp>
#include <eventstream/core/events/event_hp.hpp>
#include <eventstream/core/memory/event_pool.hpp>

using namespace std::chrono;
using namespace EventStream;

// ============================================================================
// BENCHMARK UTILITIES
// ============================================================================

struct BenchmarkMetrics {
    std::string name;
    uint64_t operations;
    double throughput_ops_sec;
    double latency_avg_ns;
    double latency_p50_ns;
    double latency_p99_ns;
    double duration_sec;
};

inline uint64_t now_ns() {
    return duration_cast<nanoseconds>(
        high_resolution_clock::now().time_since_epoch()
    ).count();
}

std::vector<BenchmarkMetrics> g_results;

void record_result(const BenchmarkMetrics& m) {
    g_results.push_back(m);
}

// ============================================================================
// BENCHMARK 1: SPSC RING BUFFER
// ============================================================================

BenchmarkMetrics benchmark_spsc() {
    std::cout << "\n[1] SpscRingBuffer (Lock-Free SPSC)" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    using TestPair = std::pair<uint64_t, uint64_t>;
    SpscRingBuffer<TestPair, 16384> buffer;
    
    const uint64_t NUM_EVENTS = 1000000;
    std::vector<uint64_t> latencies;
    latencies.reserve(NUM_EVENTS);
    
    std::atomic<bool> done{false};
    
    auto producer = [&]() {
        for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
            TestPair evt{i, now_ns()};
            while (!buffer.push(evt)) {
                std::this_thread::yield();
            }
        }
        done.store(true);
    };
    
    auto consumer = [&]() {
        while (!done.load() || buffer.SizeUsed() > 0) {
            if (auto evt = buffer.pop()) {
                uint64_t lat = now_ns() - evt->second;
                latencies.push_back(lat);
            }
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::thread prod(producer);
    std::thread cons(consumer);
    
    prod.join();
    cons.join();
    
    auto end = high_resolution_clock::now();
    double duration = duration_cast<nanoseconds>(end - start).count() / 1e9;
    
    std::sort(latencies.begin(), latencies.end());
    uint64_t sum = std::accumulate(latencies.begin(), latencies.end(), 0ULL);
    
    BenchmarkMetrics m;
    m.name = "SpscRingBuffer";
    m.operations = latencies.size();
    m.throughput_ops_sec = m.operations / duration;
    m.latency_avg_ns = static_cast<double>(sum) / latencies.size();
    m.latency_p50_ns = latencies[latencies.size() * 50 / 100];
    m.latency_p99_ns = latencies[latencies.size() * 99 / 100];
    m.duration_sec = duration;
    
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) 
              << m.throughput_ops_sec / 1e6 << " M ops/sec" << std::endl;
    std::cout << "  Latency p50: " << m.latency_p50_ns << " ns" << std::endl;
    std::cout << "  Latency p99: " << m.latency_p99_ns << " ns" << std::endl;
    
    return m;
}

// ============================================================================
// BENCHMARK 2: MPSC QUEUE
// ============================================================================

BenchmarkMetrics benchmark_mpsc(size_t num_producers = 4) {
    std::cout << "\n[2] MpscQueue (Lock-Free MPSC, " << num_producers << " producers)" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    struct TestEvent {
        uint64_t id;
        uint64_t timestamp_ns;
    };
    
    MpscQueue<TestEvent, 262144> queue;
    
    const uint64_t EVENTS_PER_PRODUCER = 250000;
    const uint64_t TOTAL_EVENTS = EVENTS_PER_PRODUCER * num_producers;
    
    std::atomic<uint64_t> events_consumed{0};
    std::atomic<bool> producers_done{false};
    std::vector<uint64_t> latencies;
    latencies.reserve(TOTAL_EVENTS);
    std::mutex lat_mutex;
    
    auto consumer = [&]() {
        while (!producers_done.load() || queue.size() > 0) {
            if (auto evt = queue.pop()) {
                uint64_t lat = now_ns() - evt->timestamp_ns;
                {
                    std::lock_guard<std::mutex> lock(lat_mutex);
                    latencies.push_back(lat);
                }
                events_consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    };
    
    auto producer = [&](size_t id) {
        for (uint64_t i = 0; i < EVENTS_PER_PRODUCER; ++i) {
            TestEvent evt{id * EVENTS_PER_PRODUCER + i, now_ns()};
            while (!queue.push(evt)) {
                std::this_thread::yield();
            }
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::thread consumer_thread(consumer);
    std::vector<std::thread> producer_threads;
    for (size_t i = 0; i < num_producers; ++i) {
        producer_threads.emplace_back(producer, i);
    }
    
    for (auto& t : producer_threads) {
        t.join();
    }
    producers_done.store(true);
    consumer_thread.join();
    
    auto end = high_resolution_clock::now();
    double duration = duration_cast<nanoseconds>(end - start).count() / 1e9;
    
    std::sort(latencies.begin(), latencies.end());
    uint64_t sum = std::accumulate(latencies.begin(), latencies.end(), 0ULL);
    
    BenchmarkMetrics m;
    m.name = "MpscQueue (" + std::to_string(num_producers) + " prod)";
    m.operations = latencies.size();
    m.throughput_ops_sec = m.operations / duration;
    m.latency_avg_ns = sum > 0 ? static_cast<double>(sum) / latencies.size() : 0;
    m.latency_p50_ns = latencies.empty() ? 0 : latencies[latencies.size() * 50 / 100];
    m.latency_p99_ns = latencies.empty() ? 0 : latencies[latencies.size() * 99 / 100];
    m.duration_sec = duration;
    
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) 
              << m.throughput_ops_sec / 1e6 << " M ops/sec" << std::endl;
    std::cout << "  Latency p50: " << m.latency_p50_ns << " ns" << std::endl;
    std::cout << "  Latency p99: " << m.latency_p99_ns << " ns" << std::endl;
    
    return m;
}

// ============================================================================
// BENCHMARK 3: LOCK-FREE DEDUPLICATOR
// ============================================================================

BenchmarkMetrics benchmark_dedup() {
    std::cout << "\n[3] LockFreeDeduplicator (CAS-based)" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    LockFreeDeduplicator dedup;
    
    const uint64_t NUM_OPS = 500000;
    uint64_t now_ms = now_ns() / 1000000;
    
    auto start = high_resolution_clock::now();
    
    for (uint64_t i = 0; i < NUM_OPS; ++i) {
        dedup.insert(i, now_ms);
    }
    
    auto mid = high_resolution_clock::now();
    
    uint64_t duplicates = 0;
    for (uint64_t i = 0; i < NUM_OPS; ++i) {
        if (dedup.is_duplicate(i, now_ms)) {
            duplicates++;
        }
    }
    
    auto end = high_resolution_clock::now();
    
    double insert_time = duration_cast<nanoseconds>(mid - start).count() / 1e9;
    double lookup_time = duration_cast<nanoseconds>(end - mid).count() / 1e9;
    double total_time = insert_time + lookup_time;
    
    BenchmarkMetrics m;
    m.name = "LockFreeDedup";
    m.operations = NUM_OPS * 2;
    m.throughput_ops_sec = m.operations / total_time;
    m.latency_avg_ns = (total_time * 1e9) / m.operations;
    m.latency_p50_ns = m.latency_avg_ns;  // Approximate
    m.latency_p99_ns = m.latency_avg_ns;
    m.duration_sec = total_time;
    
    std::cout << "  Insert: " << std::fixed << std::setprecision(2) 
              << (NUM_OPS / insert_time) / 1e6 << " M ops/sec" << std::endl;
    std::cout << "  Lookup: " << (NUM_OPS / lookup_time) / 1e6 << " M ops/sec" << std::endl;
    std::cout << "  Duplicates found: " << duplicates << "/" << NUM_OPS << std::endl;
    
    return m;
}

// ============================================================================
// BENCHMARK 4: EVENT POOL
// ============================================================================

BenchmarkMetrics benchmark_event_pool() {
    std::cout << "\n[4] EventPool (Zero-Allocation)" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    using Event = eventstream::core::HighPerformanceEvent;
    eventstream::core::EventPool<Event, 100000> pool;
    
    const uint64_t NUM_OPS = 100000;
    
    // Benchmark acquire/release cycle
    auto start = high_resolution_clock::now();
    
    for (uint64_t i = 0; i < NUM_OPS; ++i) {
        Event* evt = pool.acquire();
        evt->event_id = i;
        pool.release(evt);
    }
    
    auto end = high_resolution_clock::now();
    double duration = duration_cast<nanoseconds>(end - start).count() / 1e9;
    
    // Compare with new/delete
    auto malloc_start = high_resolution_clock::now();
    
    for (uint64_t i = 0; i < NUM_OPS; ++i) {
        Event* evt = new Event();
        evt->event_id = i;
        delete evt;
    }
    
    auto malloc_end = high_resolution_clock::now();
    double malloc_duration = duration_cast<nanoseconds>(malloc_end - malloc_start).count() / 1e9;
    
    BenchmarkMetrics m;
    m.name = "EventPool";
    m.operations = NUM_OPS;
    m.throughput_ops_sec = NUM_OPS / duration;
    m.latency_avg_ns = (duration * 1e9) / NUM_OPS;
    m.latency_p50_ns = m.latency_avg_ns;
    m.latency_p99_ns = m.latency_avg_ns;
    m.duration_sec = duration;
    
    double speedup = malloc_duration / duration;
    
    std::cout << "  Pool throughput:   " << std::fixed << std::setprecision(2) 
              << m.throughput_ops_sec / 1e6 << " M ops/sec" << std::endl;
    std::cout << "  Malloc throughput: " << (NUM_OPS / malloc_duration) / 1e6 << " M ops/sec" << std::endl;
    std::cout << "  Speedup:           " << std::setprecision(1) << speedup << "x" << std::endl;
    
    return m;
}

// ============================================================================
// BENCHMARK 5: CONCURRENT DEDUP (Multi-threaded)
// ============================================================================

BenchmarkMetrics benchmark_concurrent_dedup(size_t num_threads = 4) {
    std::cout << "\n[5] Concurrent Dedup (" << num_threads << " threads)" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    LockFreeDeduplicator dedup;
    
    const uint64_t OPS_PER_THREAD = 100000;
    const uint64_t TOTAL_OPS = OPS_PER_THREAD * num_threads;
    uint64_t now_ms = now_ns() / 1000000;
    
    std::atomic<uint64_t> success{0};
    
    auto worker = [&](size_t id) {
        for (uint64_t i = 0; i < OPS_PER_THREAD; ++i) {
            uint32_t event_id = id * OPS_PER_THREAD + i;
            if (dedup.insert(event_id, now_ms)) {
                success.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end = high_resolution_clock::now();
    double duration = duration_cast<nanoseconds>(end - start).count() / 1e9;
    
    BenchmarkMetrics m;
    m.name = "ConcurrentDedup (" + std::to_string(num_threads) + " thr)";
    m.operations = TOTAL_OPS;
    m.throughput_ops_sec = TOTAL_OPS / duration;
    m.latency_avg_ns = (duration * 1e9) / TOTAL_OPS;
    m.latency_p50_ns = m.latency_avg_ns;
    m.latency_p99_ns = m.latency_avg_ns;
    m.duration_sec = duration;
    
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) 
              << m.throughput_ops_sec / 1e6 << " M ops/sec" << std::endl;
    std::cout << "  Success rate: " << (100.0 * success.load() / TOTAL_OPS) << "%" << std::endl;
    
    return m;
}

// ============================================================================
// SUMMARY REPORT
// ============================================================================

void print_summary() {
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "                     EVENTSTREAM CORE BENCHMARK SUMMARY" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    std::cout << std::left << std::setw(30) << "Component" 
              << std::right << std::setw(15) << "Throughput"
              << std::setw(12) << "p50 (ns)"
              << std::setw(12) << "p99 (ns)"
              << std::setw(12) << "Duration"
              << std::endl;
    std::cout << std::string(80, '-') << std::endl;
    
    for (const auto& r : g_results) {
        std::cout << std::left << std::setw(30) << r.name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(12) << (r.throughput_ops_sec / 1e6) << " M/s"
                  << std::setw(12) << std::setprecision(0) << r.latency_p50_ns
                  << std::setw(12) << r.latency_p99_ns
                  << std::setw(10) << std::setprecision(3) << r.duration_sec << " s"
                  << std::endl;
    }
    
    std::cout << std::string(80, '=') << std::endl;
    
    // System info
    std::cout << "\nSystem: " << std::thread::hardware_concurrency() << " CPU cores" << std::endl;
    
    // Find best performers
    if (!g_results.empty()) {
        auto best_throughput = std::max_element(g_results.begin(), g_results.end(),
            [](const BenchmarkMetrics& a, const BenchmarkMetrics& b) {
                return a.throughput_ops_sec < b.throughput_ops_sec;
            });
        
        auto best_latency = std::min_element(g_results.begin(), g_results.end(),
            [](const BenchmarkMetrics& a, const BenchmarkMetrics& b) {
                return a.latency_p99_ns < b.latency_p99_ns;
            });
        
        std::cout << "\nBest throughput: " << best_throughput->name 
                  << " (" << std::fixed << std::setprecision(2)
                  << best_throughput->throughput_ops_sec / 1e6 << " M ops/sec)" << std::endl;
        std::cout << "Best latency:    " << best_latency->name 
                  << " (p99: " << std::setprecision(0) << best_latency->latency_p99_ns << " ns)" << std::endl;
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "\n╔════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║           EVENTSTREAM CORE - COMPREHENSIVE BENCHMARK                ║" << std::endl;
    std::cout << "║     Ultra-Low Latency Event Streaming Engine Performance Test       ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════╝" << std::endl;
    
    std::cout << "\nRunning benchmarks..." << std::endl;
    std::cout << "CPU cores available: " << std::thread::hardware_concurrency() << std::endl;
    
    // Run all benchmarks
    record_result(benchmark_spsc());
    record_result(benchmark_mpsc(2));
    record_result(benchmark_mpsc(4));
    record_result(benchmark_mpsc(8));
    record_result(benchmark_dedup());
    record_result(benchmark_event_pool());
    record_result(benchmark_concurrent_dedup(4));
    record_result(benchmark_concurrent_dedup(8));
    
    // Print summary
    print_summary();
    
    std::cout << "\n✓ All benchmarks completed successfully!" << std::endl;
    std::cout << std::endl;
    
    return 0;
}
