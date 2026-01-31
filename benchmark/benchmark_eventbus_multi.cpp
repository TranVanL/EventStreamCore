// ============================================================================
// BENCHMARK: EventBusMulti Performance
// ============================================================================
// Test EventBusMulti with multiple queue types:
// - REALTIME: SpscRingBuffer (lock-free)
// - TRANSACTIONAL: Mutex-based deque
// - BATCH: Mutex-based deque
//
// Measures:
// 1. Throughput per queue type
// 2. Latency distribution
// 3. Backpressure behavior
// 4. Cross-queue performance isolation
// ============================================================================

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <eventstream/core/events/event_bus.hpp>
#include <eventstream/core/events/event.hpp>

using namespace EventStream;
using namespace std::chrono;

// ============================================================================
// UTILITIES
// ============================================================================
inline uint64_t now_ns() {
    return duration_cast<nanoseconds>(
        high_resolution_clock::now().time_since_epoch()
    ).count();
}

struct LatencyStats {
    double min_us;
    double max_us;
    double avg_us;
    double p50_us;
    double p95_us;
    double p99_us;
};

LatencyStats compute_stats(const std::vector<uint64_t>& latencies_ns) {
    if (latencies_ns.empty()) {
        return {0, 0, 0, 0, 0, 0};
    }
    
    std::vector<uint64_t> sorted = latencies_ns;
    std::sort(sorted.begin(), sorted.end());
    
    uint64_t sum = std::accumulate(sorted.begin(), sorted.end(), 0ULL);
    auto to_us = [](uint64_t ns) { return ns / 1000.0; };
    
    return {
        to_us(sorted.front()),
        to_us(sorted.back()),
        to_us(sum / sorted.size()),
        to_us(sorted[sorted.size() * 50 / 100]),
        to_us(sorted[sorted.size() * 95 / 100]),
        to_us(sorted[sorted.size() * 99 / 100])
    };
}

void print_stats(const std::string& label, const LatencyStats& stats) {
    std::cout << label << std::endl;
    std::cout << "  min:  " << std::fixed << std::setprecision(2) << stats.min_us << " μs" << std::endl;
    std::cout << "  avg:  " << stats.avg_us << " μs" << std::endl;
    std::cout << "  p50:  " << stats.p50_us << " μs" << std::endl;
    std::cout << "  p95:  " << stats.p95_us << " μs" << std::endl;
    std::cout << "  p99:  " << stats.p99_us << " μs" << std::endl;
    std::cout << "  max:  " << stats.max_us << " μs" << std::endl;
}

EventPtr create_test_event(uint64_t id, const std::string& topic = "benchmark") {
    auto evt = std::make_shared<Event>();
    evt->header.id = static_cast<uint32_t>(id);
    evt->header.timestamp = now_ns();
    evt->header.sourceType = EventSourceType::INTERNAL;
    evt->header.priority = EventPriority::MEDIUM;
    evt->topic = topic;
    evt->body = {0x01, 0x02, 0x03, 0x04};  // Small payload
    return evt;
}

// ============================================================================
// TEST 1: REALTIME QUEUE THROUGHPUT (SPSC Lock-Free)
// ============================================================================
void test_realtime_throughput() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 1: REALTIME QUEUE THROUGHPUT (SpscRingBuffer)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    EventBusMulti bus;
    const uint64_t NUM_EVENTS = 1000000;
    
    std::atomic<uint64_t> events_consumed{0};
    std::atomic<bool> producer_done{false};
    
    std::vector<uint64_t> latencies;
    latencies.reserve(NUM_EVENTS);
    
    // Consumer thread
    auto consumer = [&]() {
        while (!producer_done.load() || bus.size(EventBusMulti::QueueId::REALTIME) > 0) {
            auto evt = bus.pop(EventBusMulti::QueueId::REALTIME, std::chrono::milliseconds(1));
            if (evt) {
                uint64_t lat = now_ns() - (*evt)->header.timestamp;
                latencies.push_back(lat);
                events_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::thread consumer_thread(consumer);
    
    // Producer
    uint64_t push_failed = 0;
    for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
        auto evt = create_test_event(i, "realtime_benchmark");
        if (!bus.push(EventBusMulti::QueueId::REALTIME, evt)) {
            push_failed++;
        }
    }
    producer_done.store(true);
    
    consumer_thread.join();
    
    auto end = high_resolution_clock::now();
    double elapsed_sec = duration_cast<nanoseconds>(end - start).count() / 1e9;
    
    std::cout << "\nResults:" << std::endl;
    std::cout << "  Events sent:     " << NUM_EVENTS << std::endl;
    std::cout << "  Events consumed: " << events_consumed.load() << std::endl;
    std::cout << "  Push failures:   " << push_failed << std::endl;
    std::cout << "  Duration:        " << std::fixed << std::setprecision(3) << elapsed_sec << " sec" << std::endl;
    std::cout << "  Throughput:      " << std::setprecision(2) 
              << (events_consumed.load() / elapsed_sec) / 1e6 << " M events/sec" << std::endl;
    
    auto stats = compute_stats(latencies);
    print_stats("\nEnd-to-end latency:", stats);
}

// ============================================================================
// TEST 2: TRANSACTIONAL QUEUE PERFORMANCE
// ============================================================================
void test_transactional_throughput() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 2: TRANSACTIONAL QUEUE THROUGHPUT (Mutex-based)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    EventBusMulti bus;
    const uint64_t NUM_EVENTS = 500000;
    
    std::atomic<uint64_t> events_consumed{0};
    std::atomic<bool> producer_done{false};
    
    std::vector<uint64_t> latencies;
    latencies.reserve(NUM_EVENTS);
    
    // Consumer thread
    auto consumer = [&]() {
        while (!producer_done.load() || bus.size(EventBusMulti::QueueId::TRANSACTIONAL) > 0) {
            auto evt = bus.pop(EventBusMulti::QueueId::TRANSACTIONAL, std::chrono::milliseconds(1));
            if (evt) {
                uint64_t lat = now_ns() - (*evt)->header.timestamp;
                latencies.push_back(lat);
                events_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::thread consumer_thread(consumer);
    
    // Producer
    for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
        auto evt = create_test_event(i, "transactional_benchmark");
        evt->header.priority = EventPriority::HIGH;
        bus.push(EventBusMulti::QueueId::TRANSACTIONAL, evt);
    }
    producer_done.store(true);
    
    consumer_thread.join();
    
    auto end = high_resolution_clock::now();
    double elapsed_sec = duration_cast<nanoseconds>(end - start).count() / 1e9;
    
    std::cout << "\nResults:" << std::endl;
    std::cout << "  Events sent:     " << NUM_EVENTS << std::endl;
    std::cout << "  Events consumed: " << events_consumed.load() << std::endl;
    std::cout << "  Duration:        " << std::fixed << std::setprecision(3) << elapsed_sec << " sec" << std::endl;
    std::cout << "  Throughput:      " << std::setprecision(2) 
              << (events_consumed.load() / elapsed_sec) / 1e6 << " M events/sec" << std::endl;
    
    auto stats = compute_stats(latencies);
    print_stats("\nEnd-to-end latency:", stats);
}

// ============================================================================
// TEST 3: BATCH QUEUE PERFORMANCE
// ============================================================================
void test_batch_throughput() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 3: BATCH QUEUE THROUGHPUT" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    EventBusMulti bus;
    const uint64_t NUM_EVENTS = 500000;
    
    std::atomic<uint64_t> events_consumed{0};
    std::atomic<bool> producer_done{false};
    
    // Consumer thread
    auto consumer = [&]() {
        while (!producer_done.load() || bus.size(EventBusMulti::QueueId::BATCH) > 0) {
            auto evt = bus.pop(EventBusMulti::QueueId::BATCH, std::chrono::milliseconds(1));
            if (evt) {
                events_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    auto start = high_resolution_clock::now();
    
    std::thread consumer_thread(consumer);
    
    // Producer
    for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
        auto evt = create_test_event(i, "batch_benchmark");
        evt->header.priority = EventPriority::BATCH;
        bus.push(EventBusMulti::QueueId::BATCH, evt);
    }
    producer_done.store(true);
    
    consumer_thread.join();
    
    auto end = high_resolution_clock::now();
    double elapsed_sec = duration_cast<nanoseconds>(end - start).count() / 1e9;
    
    std::cout << "\nResults:" << std::endl;
    std::cout << "  Events sent:     " << NUM_EVENTS << std::endl;
    std::cout << "  Events consumed: " << events_consumed.load() << std::endl;
    std::cout << "  Duration:        " << std::fixed << std::setprecision(3) << elapsed_sec << " sec" << std::endl;
    std::cout << "  Throughput:      " << std::setprecision(2) 
              << (events_consumed.load() / elapsed_sec) / 1e6 << " M events/sec" << std::endl;
}

// ============================================================================
// TEST 4: CROSS-QUEUE ISOLATION
// ============================================================================
void test_cross_queue_isolation() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 4: CROSS-QUEUE ISOLATION (All queues simultaneous)" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    EventBusMulti bus;
    const uint64_t EVENTS_PER_QUEUE = 200000;
    
    std::atomic<uint64_t> realtime_consumed{0};
    std::atomic<uint64_t> transactional_consumed{0};
    std::atomic<uint64_t> batch_consumed{0};
    std::atomic<bool> stop{false};
    
    // Consumers
    auto realtime_consumer = [&]() {
        while (!stop.load()) {
            if (bus.pop(EventBusMulti::QueueId::REALTIME, std::chrono::milliseconds(1))) {
                realtime_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    auto transactional_consumer = [&]() {
        while (!stop.load()) {
            if (bus.pop(EventBusMulti::QueueId::TRANSACTIONAL, std::chrono::milliseconds(1))) {
                transactional_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    auto batch_consumer = [&]() {
        while (!stop.load()) {
            if (bus.pop(EventBusMulti::QueueId::BATCH, std::chrono::milliseconds(1))) {
                batch_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    // Producers
    auto realtime_producer = [&]() {
        for (uint64_t i = 0; i < EVENTS_PER_QUEUE; ++i) {
            auto evt = create_test_event(i, "realtime");
            bus.push(EventBusMulti::QueueId::REALTIME, evt);
        }
    };
    
    auto transactional_producer = [&]() {
        for (uint64_t i = 0; i < EVENTS_PER_QUEUE; ++i) {
            auto evt = create_test_event(i, "transactional");
            bus.push(EventBusMulti::QueueId::TRANSACTIONAL, evt);
        }
    };
    
    auto batch_producer = [&]() {
        for (uint64_t i = 0; i < EVENTS_PER_QUEUE; ++i) {
            auto evt = create_test_event(i, "batch");
            bus.push(EventBusMulti::QueueId::BATCH, evt);
        }
    };
    
    auto start = high_resolution_clock::now();
    
    // Start all threads
    std::thread rc(realtime_consumer);
    std::thread tc(transactional_consumer);
    std::thread bc(batch_consumer);
    
    std::thread rp(realtime_producer);
    std::thread tp(transactional_producer);
    std::thread bp(batch_producer);
    
    rp.join();
    tp.join();
    bp.join();
    
    // Wait for consumers to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    
    rc.join();
    tc.join();
    bc.join();
    
    auto end = high_resolution_clock::now();
    double elapsed_sec = duration_cast<nanoseconds>(end - start).count() / 1e9;
    
    uint64_t total = realtime_consumed.load() + transactional_consumed.load() + batch_consumed.load();
    
    std::cout << "\nResults:" << std::endl;
    std::cout << "  Realtime:      " << realtime_consumed.load() << " events" << std::endl;
    std::cout << "  Transactional: " << transactional_consumed.load() << " events" << std::endl;
    std::cout << "  Batch:         " << batch_consumed.load() << " events" << std::endl;
    std::cout << "  Total:         " << total << " events" << std::endl;
    std::cout << "  Duration:      " << std::fixed << std::setprecision(3) << elapsed_sec << " sec" << std::endl;
    std::cout << "  Combined throughput: " << std::setprecision(2) 
              << (total / elapsed_sec) / 1e6 << " M events/sec" << std::endl;
}

// ============================================================================
// TEST 5: BACKPRESSURE BEHAVIOR
// ============================================================================
void test_backpressure() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST 5: BACKPRESSURE BEHAVIOR" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    
    EventBusMulti bus;
    const uint64_t NUM_EVENTS = 50000;  // More than REALTIME buffer (16384)
    
    // Push without consumer - test buffer overflow handling
    uint64_t push_success = 0;
    uint64_t push_failed = 0;
    
    for (uint64_t i = 0; i < NUM_EVENTS; ++i) {
        auto evt = create_test_event(i, "backpressure_test");
        if (bus.push(EventBusMulti::QueueId::REALTIME, evt)) {
            push_success++;
        } else {
            push_failed++;
        }
    }
    
    std::cout << "\nRealtime queue (capacity: 16384):" << std::endl;
    std::cout << "  Attempted:  " << NUM_EVENTS << std::endl;
    std::cout << "  Succeeded:  " << push_success << std::endl;
    std::cout << "  Dropped:    " << push_failed << std::endl;
    std::cout << "  Queue size: " << bus.size(EventBusMulti::QueueId::REALTIME) << std::endl;
    
    auto pressure = bus.getRealtimePressure();
    std::cout << "  Pressure:   ";
    switch (pressure) {
        case EventBusMulti::PressureLevel::NORMAL: std::cout << "NORMAL"; break;
        case EventBusMulti::PressureLevel::HIGH: std::cout << "HIGH"; break;
        case EventBusMulti::PressureLevel::CRITICAL: std::cout << "CRITICAL"; break;
    }
    std::cout << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    std::cout << "\n╔════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  EventBusMulti PERFORMANCE BENCHMARK                                ║" << std::endl;
    std::cout << "║  Multi-Queue Event Bus with Priority Support                        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════╝" << std::endl;
    
    test_realtime_throughput();
    test_transactional_throughput();
    test_batch_throughput();
    test_cross_queue_isolation();
    test_backpressure();
    
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "All EventBusMulti benchmarks completed!" << std::endl;
    std::cout << std::string(70, '=') << "\n" << std::endl;
    
    return 0;
}
