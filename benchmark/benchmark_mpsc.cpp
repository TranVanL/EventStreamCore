// ============================================================================
// BENCHMARK: LOCK-FREE MPSC QUEUE PERFORMANCE
// ============================================================================
// Performance measurement for Multi-Producer Single-Consumer Queue
//
// Scenarios:
// 1. Sequential push/pop (baseline)
// 2. Concurrent multi-producer (2, 4, 8 threads)
// 3. Throughput under contention
// 4. Latency distribution
// ============================================================================

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <eventstream/core/queues/mpsc_queue.hpp>

// ============================================================================
// TEST EVENT
// ============================================================================
struct TestEvent {
    uint64_t id;
    uint64_t producer_id;
    uint64_t timestamp_ns;
    
    TestEvent() : id(0), producer_id(0), timestamp_ns(0) {}
    TestEvent(uint64_t id_, uint64_t pid, uint64_t ts)
        : id(id_), producer_id(pid), timestamp_ns(ts) {}
};

// ============================================================================
// LATENCY STATISTICS
// ============================================================================
struct LatencyStats {
    double min_ns;
    double max_ns;
    double avg_ns;
    double p50_ns;
    double p95_ns;
    double p99_ns;
};

LatencyStats compute_stats(const std::vector<uint64_t>& latencies) {
    if (latencies.empty()) {
        return {0, 0, 0, 0, 0, 0};
    }
    
    std::vector<uint64_t> sorted = latencies;
    std::sort(sorted.begin(), sorted.end());
    
    uint64_t sum = std::accumulate(sorted.begin(), sorted.end(), 0ULL);
    
    return {
        static_cast<double>(sorted.front()),
        static_cast<double>(sorted.back()),
        static_cast<double>(sum) / sorted.size(),
        static_cast<double>(sorted[sorted.size() * 50 / 100]),
        static_cast<double>(sorted[sorted.size() * 95 / 100]),
        static_cast<double>(sorted[sorted.size() * 99 / 100])
    };
}

void print_stats(const std::string& label, const LatencyStats& stats) {
    std::cout << label << std::endl;
    std::cout << "  min:  " << std::fixed << std::setprecision(1) << stats.min_ns << " ns" << std::endl;
    std::cout << "  avg:  " << stats.avg_ns << " ns" << std::endl;
    std::cout << "  p50:  " << stats.p50_ns << " ns" << std::endl;
    std::cout << "  p95:  " << stats.p95_ns << " ns" << std::endl;
    std::cout << "  p99:  " << stats.p99_ns << " ns" << std::endl;
    std::cout << "  max:  " << stats.max_ns << " ns" << std::endl;
}

inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// TEST 1: SEQUENTIAL THROUGHPUT (BASELINE)
// ============================================================================
void test_sequential_throughput() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 1: SEQUENTIAL THROUGHPUT (Single Thread Baseline)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    MpscQueue<TestEvent, 65536> queue;
    const uint64_t NUM_EVENTS = 1000000;
    
    // Push all events
    auto push_start = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
        TestEvent evt(i, 0, now_ns());
        queue.push(evt);
    }
    auto push_end = std::chrono::high_resolution_clock::now();
    
    uint64_t push_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(push_end - push_start).count();
    
    // Pop all events
    auto pop_start = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
        auto evt = queue.pop();
        if (!evt) break;
    }
    auto pop_end = std::chrono::high_resolution_clock::now();
    
    uint64_t pop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(pop_end - pop_start).count();
    
    std::cout << "\nPush performance:" << std::endl;
    std::cout << "  Total time:  " << push_ns / 1000000.0 << " ms" << std::endl;
    std::cout << "  Throughput:  " << std::fixed << std::setprecision(2) 
              << (NUM_EVENTS * 1e9 / push_ns) / 1e6 << " M events/sec" << std::endl;
    std::cout << "  Per-op:      " << std::setprecision(1) << (double)push_ns / NUM_EVENTS << " ns" << std::endl;
    
    std::cout << "\nPop performance:" << std::endl;
    std::cout << "  Total time:  " << pop_ns / 1000000.0 << " ms" << std::endl;
    std::cout << "  Throughput:  " << std::fixed << std::setprecision(2) 
              << (NUM_EVENTS * 1e9 / pop_ns) / 1e6 << " M events/sec" << std::endl;
    std::cout << "  Per-op:      " << std::setprecision(1) << (double)pop_ns / NUM_EVENTS << " ns" << std::endl;
}

// ============================================================================
// TEST 2: CONCURRENT MULTI-PRODUCER THROUGHPUT
// ============================================================================
void test_concurrent_producers(size_t num_producers) {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 2: CONCURRENT PRODUCERS (" << num_producers << " threads)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    MpscQueue<TestEvent, 1048576> queue;  // 1M capacity
    const uint64_t EVENTS_PER_PRODUCER = 500000;
    const uint64_t TOTAL_EVENTS = EVENTS_PER_PRODUCER * num_producers;
    
    std::atomic<uint64_t> events_pushed{0};
    std::atomic<uint64_t> events_popped{0};
    std::atomic<bool> producers_done{false};
    
    // Consumer thread
    auto consumer = [&]() {
        while (!producers_done.load() || queue.size() > 0) {
            if (auto evt = queue.pop()) {
                events_popped.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        // Drain remaining
        while (auto evt = queue.pop()) {
            events_popped.fetch_add(1, std::memory_order_relaxed);
        }
    };
    
    // Producer threads
    auto producer = [&](size_t thread_id) {
        for (uint64_t i = 0; i < EVENTS_PER_PRODUCER; ++i) {
            TestEvent evt(thread_id * EVENTS_PER_PRODUCER + i, thread_id, now_ns());
            while (!queue.push(evt)) {
                std::this_thread::yield();
            }
            events_pushed.fetch_add(1, std::memory_order_relaxed);
        }
    };
    
    auto start = std::chrono::high_resolution_clock::now();
    
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
    
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_sec = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1e9;
    
    std::cout << "\nResults:" << std::endl;
    std::cout << "  Producers:    " << num_producers << std::endl;
    std::cout << "  Total pushed: " << events_pushed.load() << std::endl;
    std::cout << "  Total popped: " << events_popped.load() << std::endl;
    std::cout << "  Duration:     " << std::fixed << std::setprecision(3) << elapsed_sec << " sec" << std::endl;
    std::cout << "  Throughput:   " << std::setprecision(2) 
              << (TOTAL_EVENTS / elapsed_sec) / 1e6 << " M events/sec" << std::endl;
}

// ============================================================================
// TEST 3: END-TO-END LATENCY
// ============================================================================
void test_latency() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 3: END-TO-END LATENCY (1 producer, 1 consumer)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    MpscQueue<TestEvent, 65536> queue;
    const uint64_t NUM_EVENTS = 100000;
    
    std::vector<uint64_t> latencies;
    latencies.reserve(NUM_EVENTS);
    
    std::atomic<bool> done{false};
    
    // Consumer measures latency
    auto consumer = [&]() {
        while (!done.load() || queue.size() > 0) {
            if (auto evt = queue.pop()) {
                uint64_t lat = now_ns() - evt->timestamp_ns;
                latencies.push_back(lat);
            } else {
                std::this_thread::yield();
            }
        }
    };
    
    // Producer
    auto producer = [&]() {
        for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
            TestEvent evt(i, 0, now_ns());
            while (!queue.push(evt)) {
                std::this_thread::yield();
            }
        }
        done.store(true);
    };
    
    std::thread consumer_thread(consumer);
    std::thread producer_thread(producer);
    
    producer_thread.join();
    consumer_thread.join();
    
    auto stats = compute_stats(latencies);
    print_stats("\nEnd-to-end latency:", stats);
}

// ============================================================================
// TEST 4: CONTENTION STRESS TEST
// ============================================================================
void test_contention() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 4: HIGH CONTENTION STRESS (8 producers burst mode)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    MpscQueue<TestEvent, 262144> queue;  // 256K capacity
    const size_t NUM_PRODUCERS = 8;
    const uint64_t EVENTS_PER_BURST = 10000;
    const size_t NUM_BURSTS = 10;
    
    std::atomic<uint64_t> push_success{0};
    std::atomic<uint64_t> push_failed{0};
    std::atomic<uint64_t> events_consumed{0};
    std::atomic<bool> stop{false};
    
    // Consumer - steady drain
    auto consumer = [&]() {
        while (!stop.load()) {
            if (auto evt = queue.pop()) {
                events_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Drain remaining
        while (auto evt = queue.pop()) {
            events_consumed.fetch_add(1, std::memory_order_relaxed);
        }
    };
    
    // Bursty producers
    auto producer = [&](size_t id) {
        for (size_t burst = 0; burst < NUM_BURSTS; ++burst) {
            // Burst: push as fast as possible
            for (uint64_t i = 0; i < EVENTS_PER_BURST; ++i) {
                TestEvent evt(id * 1000000 + burst * EVENTS_PER_BURST + i, id, now_ns());
                if (queue.push(evt)) {
                    push_success.fetch_add(1, std::memory_order_relaxed);
                } else {
                    push_failed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Small pause between bursts
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::thread consumer_thread(consumer);
    std::vector<std::thread> producer_threads;
    for (size_t i = 0; i < NUM_PRODUCERS; ++i) {
        producer_threads.emplace_back(producer, i);
    }
    
    for (auto& t : producer_threads) {
        t.join();
    }
    
    // Wait for consumer to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    consumer_thread.join();
    
    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    uint64_t total_attempted = push_success.load() + push_failed.load();
    
    std::cout << "\nResults:" << std::endl;
    std::cout << "  Total attempted: " << total_attempted << std::endl;
    std::cout << "  Push success:    " << push_success.load() 
              << " (" << std::fixed << std::setprecision(1) 
              << (100.0 * push_success.load() / total_attempted) << "%)" << std::endl;
    std::cout << "  Push failed:     " << push_failed.load() << std::endl;
    std::cout << "  Consumed:        " << events_consumed.load() << std::endl;
    std::cout << "  Duration:        " << elapsed_ms << " ms" << std::endl;
    std::cout << "  Throughput:      " << std::setprecision(2)
              << (push_success.load() / (elapsed_ms / 1000.0)) / 1e6 << " M events/sec" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    std::cout << "\n╔════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  MPSC LOCK-FREE QUEUE PERFORMANCE BENCHMARK                         ║" << std::endl;
    std::cout << "║  Multi-Producer Single-Consumer Queue (Vyukov Algorithm)            ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════╝" << std::endl;
    
    test_sequential_throughput();
    
    test_concurrent_producers(2);
    test_concurrent_producers(4);
    test_concurrent_producers(8);
    
    test_latency();
    test_contention();
    
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "All MPSC benchmarks completed!" << std::endl;
    std::cout << std::string(70, '=') << "\n" << std::endl;
    
    return 0;
}
