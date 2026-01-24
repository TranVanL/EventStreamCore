// ============================================================================
// BENCHMARK: LOCK-FREE DEDUPLICATOR vs MUTEX-BASED
// ============================================================================
// Day 35: Performance comparison of optimized dedup implementation
//
// Test scenarios:
// 1. Sequential insertion (single thread)
// 2. Concurrent insertion (multiple threads)
// 3. Duplicate detection (read-heavy)
// 4. Cleanup operations
// ============================================================================

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <unordered_set>
#include <mutex>
#include <iomanip>
#include "utils/lock_free_dedup.hpp"

using namespace EventStream;

// ============================================================================
// MUTEX-BASED BASELINE (for comparison)
// ============================================================================
class MutexBasedDedup {
private:
    struct Entry {
        uint32_t id;
        uint64_t timestamp_ms;
    };
    
    mutable std::mutex mutex_;
    std::unordered_set<uint32_t> entries_;
    std::unordered_map<uint32_t, uint64_t> timestamps_;
    
public:
    bool is_duplicate(uint32_t id, uint64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.find(id) != entries_.end();
    }
    
    bool insert(uint32_t id, uint64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.find(id) != entries_.end()) {
            return false;  // Already exists
        }
        entries_.insert(id);
        timestamps_[id] = now_ms;
        return true;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }
};

// ============================================================================
// BENCHMARK UTILITIES
// ============================================================================

struct BenchmarkResult {
    std::string name;
    uint64_t total_ops;
    uint64_t elapsed_ns;
    double ops_per_sec;
    double ns_per_op;
};

void print_header(const std::string& test_name) {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST: " << test_name << std::endl;
    std::cout << std::string(70, '=') << std::endl;
}

void print_result(const BenchmarkResult& r) {
    std::cout << std::left << std::setw(30) << r.name
              << std::right
              << std::setw(12) << r.total_ops << " ops | "
              << std::setw(10) << std::fixed << std::setprecision(2) << (r.ops_per_sec / 1e6) << "M ops/s | "
              << std::setw(8) << std::fixed << std::setprecision(1) << r.ns_per_op << " ns/op"
              << std::endl;
}

template<typename Dedup>
BenchmarkResult benchmark_sequential_insert(const std::string& name, uint64_t num_ops) {
    Dedup dedup;
    uint64_t now_ms = std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (uint64_t i = 0; i < num_ops; ++i) {
        dedup.insert(i, now_ms);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    return {name, num_ops, elapsed_ns, (num_ops * 1e9) / elapsed_ns, (double)elapsed_ns / num_ops};
}

template<typename Dedup>
BenchmarkResult benchmark_duplicate_detection(const std::string& name, uint64_t num_inserts, uint64_t num_lookups) {
    Dedup dedup;
    uint64_t now_ms = std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    
    // Pre-populate
    for (uint64_t i = 0; i < num_inserts; ++i) {
        dedup.insert(i, now_ms);
    }
    
    // Benchmark lookups
    auto start = std::chrono::high_resolution_clock::now();
    
    for (uint64_t i = 0; i < num_lookups; ++i) {
        uint32_t id = i % num_inserts;
        volatile bool result = dedup.is_duplicate(id, now_ms);
        (void)result;  // Prevent optimization
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    return {name, num_lookups, elapsed_ns, (num_lookups * 1e9) / elapsed_ns, (double)elapsed_ns / num_lookups};
}

template<typename Dedup>
BenchmarkResult benchmark_concurrent_insert(const std::string& name, uint64_t threads, uint64_t ops_per_thread) {
    Dedup dedup;
    uint64_t now_ms = std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> thread_vec;
    for (uint64_t t = 0; t < threads; ++t) {
        thread_vec.emplace_back([&, t]() {
            for (uint64_t i = 0; i < ops_per_thread; ++i) {
                uint32_t id = t * 1000000 + i;
                dedup.insert(id, now_ms);
            }
        });
    }
    
    for (auto& t : thread_vec) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    uint64_t total_ops = threads * ops_per_thread;
    
    return {name, total_ops, elapsed_ns, (total_ops * 1e9) / elapsed_ns, (double)elapsed_ns / total_ops};
}

// ============================================================================
// TEST SUITES
// ============================================================================

void test_sequential_insertion() {
    print_header("Sequential Insertion (100K operations)");
    
    const uint64_t NUM_OPS = 100000;
    
    auto result_mutex = benchmark_sequential_insert<MutexBasedDedup>("Mutex-based", NUM_OPS);
    auto result_lockfree = benchmark_sequential_insert<LockFreeDeduplicator>("Lock-free", NUM_OPS);
    
    print_result(result_mutex);
    print_result(result_lockfree);
    
    double speedup = (double)result_mutex.ns_per_op / result_lockfree.ns_per_op;
    std::cout << "\n" << "Speedup: " << std::fixed << std::setprecision(2) << speedup << "x faster" << std::endl;
}

void test_duplicate_detection() {
    print_header("Duplicate Detection (1M reads, 10K pre-inserted)");
    
    const uint64_t NUM_INSERTS = 10000;
    const uint64_t NUM_LOOKUPS = 1000000;
    
    auto result_mutex = benchmark_duplicate_detection<MutexBasedDedup>(
        "Mutex-based", NUM_INSERTS, NUM_LOOKUPS);
    auto result_lockfree = benchmark_duplicate_detection<LockFreeDeduplicator>(
        "Lock-free", NUM_INSERTS, NUM_LOOKUPS);
    
    print_result(result_mutex);
    print_result(result_lockfree);
    
    double speedup = (double)result_mutex.ns_per_op / result_lockfree.ns_per_op;
    std::cout << "\n" << "Speedup: " << std::fixed << std::setprecision(2) << speedup << "x faster" << std::endl;
}

void test_concurrent_insertion() {
    print_header("Concurrent Insertion (4 threads, 50K ops each)");
    
    const uint64_t THREADS = 4;
    const uint64_t OPS_PER_THREAD = 50000;
    
    auto result_mutex = benchmark_concurrent_insert<MutexBasedDedup>(
        "Mutex-based (4 threads)", THREADS, OPS_PER_THREAD);
    auto result_lockfree = benchmark_concurrent_insert<LockFreeDeduplicator>(
        "Lock-free (4 threads)", THREADS, OPS_PER_THREAD);
    
    print_result(result_mutex);
    print_result(result_lockfree);
    
    double speedup = (double)result_mutex.ns_per_op / result_lockfree.ns_per_op;
    std::cout << "\n" << "Speedup: " << std::fixed << std::setprecision(2) << speedup << "x faster" << std::endl;
}

void test_high_contention() {
    print_header("High Contention (8 threads, same ID)");
    
    LockFreeDeduplicator dedup;
    uint64_t now_ms = std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    std::atomic<uint64_t> successful_inserts{0};
    
    const uint64_t THREADS = 8;
    const uint64_t OPS_PER_THREAD = 10000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<std::thread> thread_vec;
    for (uint64_t t = 0; t < THREADS; ++t) {
        thread_vec.emplace_back([&]() {
            for (uint64_t i = 0; i < OPS_PER_THREAD; ++i) {
                if (dedup.insert(12345, now_ms)) {  // Same ID for all threads
                    successful_inserts.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    
    for (auto& t : thread_vec) {
        t.join();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "Total operations: " << (THREADS * OPS_PER_THREAD) << std::endl;
    std::cout << "Successful inserts: " << successful_inserts.load() << std::endl;
    std::cout << "Time: " << elapsed_ms << " ms" << std::endl;
    std::cout << "Failed (duplicate): " << (THREADS * OPS_PER_THREAD - successful_inserts.load()) << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n╔════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  LOCK-FREE DEDUPLICATOR PERFORMANCE BENCHMARK                       ║" << std::endl;
    std::cout << "║  Day 35: Optimize EventStreamCore                                    ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════╝" << std::endl;
    
    test_sequential_insertion();
    test_duplicate_detection();
    test_concurrent_insertion();
    test_high_contention();
    
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "All benchmarks completed successfully!" << std::endl;
    std::cout << std::string(70, '=') << "\n" << std::endl;
    
    return 0;
}
