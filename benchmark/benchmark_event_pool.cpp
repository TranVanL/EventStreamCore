#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <iomanip>
#ifdef __x86_64__
#include <x86intrin.h>  // for rdtsc on x86
#endif
#include <eventstream/core/memory/event_pool.hpp>
#include <eventstream/core/events/event_hp.hpp>
#include <eventstream/core/queues/spsc_ring_buffer.hpp>
#include <eventstream/core/ingest/ingest_pool.hpp>  // Production pool

using namespace eventstream::core;  // EventPool, HighPerformanceEvent
using Event = HighPerformanceEvent;
using EventQueue = SpscRingBuffer<Event*, 16384>;

// Cross-platform high-resolution timestamp
#ifdef __x86_64__
static inline uint64_t get_timestamp() {
    return __rdtsc();
}
#else
static inline uint64_t get_timestamp() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}
#endif

/**
 * Benchmark: Memory allocation overhead comparison
 * 
 * Test 1: Without event pool (naive approach)
 *   - Allocate new event for each push
 *   - Delete after processing
 *   - High allocator contention
 * 
 * Test 2: With per-thread event pool
 *   - Reuse pre-allocated events
 *   - No allocation in fast path
 *   - Stable latency
 * 
 * NOTE: Using high-resolution timestamps for accurate measurement
 */

// Get TSC (clock cycle counter) - platform independent wrapper
static inline uint64_t rdtsc() {
    return get_timestamp();
}

// Test without pool - baseline
void benchmark_without_pool(int iterations) {
    std::cout << "  Starting without-pool test..." << std::endl;
    
    EventQueue queue;
    
    // Warm up
    for (int i = 0; i < 100; ++i) {
        Event* evt = new Event();
        queue.push(evt);
        if (auto e = queue.pop()) {
            delete e.value();
        }
    }
    
    // Actual benchmark - measure ONLY allocation/deallocation
    auto start_tsc = rdtsc();
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        Event* evt = new Event();  // ALLOCATION
        evt->event_id = i;
        
        queue.push(evt);
        
        if (auto evt_opt = queue.pop()) {
            Event* e = evt_opt.value();
            delete e;  // DEALLOCATION
        }
        
        if (i % 20000 == 0 && i > 0) {
            std::cout << "    Progress: " << i << "/" << iterations << std::endl;
        }
    }
    
    auto elapsed_time = std::chrono::high_resolution_clock::now() - start_time;
    
    std::cout << "\n=== WITHOUT EVENT POOL ===" << std::endl;
    std::cout << "Iterations:       " << iterations << std::endl;
    std::cout << "Total time:       " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time).count() << " ms" << std::endl;
    auto ns_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_time).count();
    std::cout << "Throughput:       " << (iterations * 1000000000LL) / ns_elapsed << " ops/sec" << std::endl;
    std::cout << "Avg per op:       " << ns_elapsed / iterations << " ns" << std::endl;
}

// Test with pool - optimized
void benchmark_with_pool(int iterations) {
    std::cout << "  Starting with-pool test..." << std::endl;
    
    EventQueue queue;
    EventPool<Event, 1000000> pool;  // Static capacity at compile time
    
    // Warm up
    for (int i = 0; i < 100; ++i) {
        Event* evt = pool.acquire();
        queue.push(evt);
        if (auto e = queue.pop()) {
            pool.release(e.value());
        }
    }
    
    // Actual benchmark - NO timestamp overhead
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        Event* evt = pool.acquire();  // O(1) - just decrement counter
        evt->event_id = i;
        
        queue.push(evt);
        
        if (auto evt_opt = queue.pop()) {
            Event* e = evt_opt.value();
            pool.release(e);  // O(1) - just increment counter
        }
        
        if (i % 20000 == 0 && i > 0) {
            std::cout << "    Progress: " << i << "/" << iterations << std::endl;
        }
    }
    
    auto elapsed_time = std::chrono::high_resolution_clock::now() - start_time;
    
    std::cout << "\n=== WITH EVENT POOL (STATIC ARRAY) ===" << std::endl;
    std::cout << "Iterations:       " << iterations << std::endl;
    std::cout << "Total time:       " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time).count() << " ms" << std::endl;
    auto ns_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_time).count();
    std::cout << "Throughput:       " << (iterations * 1000000000LL) / ns_elapsed << " ops/sec" << std::endl;
    std::cout << "Avg per op:       " << ns_elapsed / iterations << " ns" << std::endl;
    std::cout << "Pool utilization: " << pool.utilization_percent() << "%" << std::endl;
}

// Test with IngestEventPool - production thread-safe pool
void benchmark_ingest_pool(int iterations) {
    std::cout << "  Starting IngestEventPool test (production)..." << std::endl;
    
    // Initialize pool
    EventStream::IngestEventPool::initialize();
    
    // Warm up
    for (int i = 0; i < 100; ++i) {
        auto evt = EventStream::IngestEventPool::acquireEvent();
        // shared_ptr auto-releases when goes out of scope
    }
    
    // Actual benchmark
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        auto evt = EventStream::IngestEventPool::acquireEvent();  // Thread-safe acquire
        evt->header.id = i;
        // evt auto-returns to pool when shared_ptr destructs
        
        if (i % 100000 == 0 && i > 0) {
            std::cout << "    Progress: " << i << "/" << iterations << std::endl;
        }
    }
    
    auto elapsed_time = std::chrono::high_resolution_clock::now() - start_time;
    
    std::cout << "\n=== WITH INGEST EVENT POOL (PRODUCTION) ===" << std::endl;
    std::cout << "Iterations:       " << iterations << std::endl;
    std::cout << "Total time:       " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time).count() << " ms" << std::endl;
    auto ns_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_time).count();
    std::cout << "Throughput:       " << (iterations * 1000000000LL) / ns_elapsed << " ops/sec" << std::endl;
    std::cout << "Avg per op:       " << ns_elapsed / iterations << " ns (includes mutex + shared_ptr)" << std::endl;
    std::cout << "Pool size:        " << EventStream::IngestEventPool::getPoolSize() << std::endl;
    
    // Shutdown pool
    EventStream::IngestEventPool::shutdown();
}

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  EVENT MEMORY POOL BENCHMARK                               ║" << std::endl;
    std::cout << "║  Comparing malloc/free vs object pool for event allocation ║" << std::endl;
    std::cout << "╠════════════════════════════════════════════════════════════╣" << std::endl;
    std::cout << "║  NOTE: This benchmark uses EventPool (single-thread only)  ║" << std::endl;
    std::cout << "║  Production code uses IngestEventPool (thread-safe)        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;
    
    std::cout << "\nEvent struct info:" << std::endl;
    std::cout << "  Size:      " << sizeof(Event) << " bytes" << std::endl;
    std::cout << "  Alignment: " << alignof(Event) << " bytes (cache-line)" << std::endl;
    std::cout << "  Payload:   " << Event::PAYLOAD_SIZE << " bytes (fixed)" << std::endl;
    
    int iterations = 1000000;  // 1M events for meaningful results
    std::cout << "\nRunning with " << iterations << " events..." << std::endl;
    
    // Actual benchmark (no warmup needed)
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "ACTUAL BENCHMARK (1,000,000 events)" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    benchmark_without_pool(iterations);
    benchmark_with_pool(iterations);
    benchmark_ingest_pool(iterations / 10);  // Fewer iterations for thread-safe pool
    
    // Summary
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "ANALYSIS" << std::endl;
    std::cout << std::string(60, '-') << std::endl;
    std::cout << "EventPool (this benchmark):" << std::endl;
    std::cout << "  ✓ O(1) acquire/release with zero allocation" << std::endl;
    std::cout << "  ✓ Best for single-thread or per-thread usage" << std::endl;
    std::cout << "  ✗ NOT thread-safe for cross-thread event passing" << std::endl;
    std::cout << std::endl;
    std::cout << "IngestEventPool (production):" << std::endl;
    std::cout << "  ✓ Thread-safe with mutex (~50ns overhead)" << std::endl;
    std::cout << "  ✓ Returns shared_ptr with auto-return to pool" << std::endl;
    std::cout << "  ✓ Safe for TCP → Dispatcher → Processor pipeline" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    return 0;
}
