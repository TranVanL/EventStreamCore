// ============================================================================
// EVENTSTREAM CORE - DAY 32 PERFORMANCE BENCHMARK (FIXED CLOCK)
// ============================================================================
// Day 32 Improvements:
// 1. Use steady_clock (monotonic, not affected by NTP/OS scheduling)
// 2. Split latency measurement (queue_wait, processing, end_to_end)
// 3. Prepare for profiling analysis
//
// Scenarios:
//   - SPSC: 1 producer → 1 queue → 1 processor
//   - MPSC: N producers → 1 queue → 1 processor  
//   - MPMC: N producers → 1 queue → M processors
// ============================================================================

#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <cstring>
#include <cmath>

using namespace std;
using namespace std::chrono;

// ============================================================================
// FIXED MONOTONIC CLOCK (steady_clock)
// ============================================================================

class Clock {
public:
    // Get current time in nanoseconds (monotonic)
    static inline uint64_t now_ns() {
        auto now = steady_clock::now();
        return duration_cast<nanoseconds>(now.time_since_epoch()).count();
    }
    
    // Get current time in microseconds
    static inline uint64_t now_us() {
        return now_ns() / 1000;
    }
};

// ============================================================================
// SCENARIO ENUM & CONFIGURATION
// ============================================================================

enum class Scenario {
    SPSC,   // Single Producer, Single Consumer
    MPSC,   // Multi Producer, Single Consumer
    MPMC    // Multi Producer, Multi Consumer
};

const char* scenario_name(Scenario s) {
    switch (s) {
        case Scenario::SPSC: return "SPSC";
        case Scenario::MPSC: return "MPSC";
        case Scenario::MPMC: return "MPMC";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// EVENT WITH SPLIT LATENCY TIMESTAMPS
// ============================================================================

struct BenchmarkEvent {
    uint32_t id;
    uint64_t ingest_ts;         // When event was created by producer
    uint64_t dequeue_ts;        // When event was dequeued by consumer
    uint64_t process_done_ts;   // When consumer finished processing
    uint32_t payload_size;
    
    BenchmarkEvent() 
        : id(0), ingest_ts(0), dequeue_ts(0), process_done_ts(0), payload_size(0) {}
    
    BenchmarkEvent(uint32_t id_, uint32_t payload_sz)
        : id(id_), ingest_ts(Clock::now_ns()), 
          dequeue_ts(0), process_done_ts(0), payload_size(payload_sz) {}
    
    // Calculate latencies
    uint64_t queue_wait_latency_ns() const {
        return dequeue_ts - ingest_ts;
    }
    
    uint64_t processing_latency_ns() const {
        return process_done_ts - dequeue_ts;
    }
    
    uint64_t end_to_end_latency_ns() const {
        return process_done_ts - ingest_ts;
    }
};

// ============================================================================
// LATENCY MEASUREMENT STRUCTURE
// ============================================================================

struct LatencyMetrics {
    vector<uint64_t> queue_wait;
    vector<uint64_t> processing;
    vector<uint64_t> end_to_end;
    
    void reserve(size_t capacity) {
        queue_wait.reserve(capacity);
        processing.reserve(capacity);
        end_to_end.reserve(capacity);
    }
    
    void add_latencies(const BenchmarkEvent& evt) {
        queue_wait.push_back(evt.queue_wait_latency_ns());
        processing.push_back(evt.processing_latency_ns());
        end_to_end.push_back(evt.end_to_end_latency_ns());
    }
    
    struct Stats {
        double p50, p95, p99, mean, max;
    };
    
    Stats calculate_stats(const vector<uint64_t>& latencies) const {
        if (latencies.empty()) {
            return {0, 0, 0, 0, 0};
        }
        
        vector<uint64_t> sorted = latencies;
        sort(sorted.begin(), sorted.end());
        
        uint64_t sum = 0;
        uint64_t max_val = 0;
        for (auto lat : sorted) {
            sum += lat;
            max_val = max(max_val, lat);
        }
        
        return {
            (double)sorted[sorted.size() * 50 / 100],
            (double)sorted[sorted.size() * 95 / 100],
            (double)sorted[sorted.size() * 99 / 100],
            (double)sum / sorted.size(),
            (double)max_val
        };
    }
};

// ============================================================================
// BENCHMARK RESULT STRUCT
// ============================================================================

struct BenchmarkResult {
    Scenario scenario;
    double throughput;          // events/sec
    LatencyMetrics::Stats queue_wait_stats;
    LatencyMetrics::Stats processing_stats;
    LatencyMetrics::Stats end_to_end_stats;
    uint64_t total_events;
    uint64_t processed_events;
    double duration_sec;
    int num_producers;
    int num_consumers;
    
    BenchmarkResult()
        : scenario(Scenario::SPSC), throughput(0), total_events(0),
          processed_events(0), duration_sec(0),
          num_producers(1), num_consumers(1) {}
    
    // Format output for human reading
    string to_string() const {
        ostringstream oss;
        oss << fixed << setprecision(2);
        oss << "\n[Scenario: " << scenario_name(scenario) << "]\n";
        oss << "Threads: " << num_producers << " producers / " << num_consumers << " consumers\n";
        oss << "Total events: " << total_events << "\n";
        oss << "Processed: " << processed_events << "\n";
        oss << "Duration: " << duration_sec << " sec\n";
        oss << "Throughput: " << (throughput / 1e6) << "M events/sec\n\n";
        
        // Queue Wait Latency
        oss << "Queue Wait Latency (ns):\n";
        oss << "  p50=" << setprecision(0) << queue_wait_stats.p50 
            << " p95=" << queue_wait_stats.p95 
            << " p99=" << queue_wait_stats.p99
            << " mean=" << setprecision(2) << queue_wait_stats.mean << "\n";
        
        // Processing Latency
        oss << "Processing Latency (ns):\n";
        oss << "  p50=" << setprecision(0) << processing_stats.p50 
            << " p95=" << processing_stats.p95 
            << " p99=" << processing_stats.p99
            << " mean=" << setprecision(2) << processing_stats.mean << "\n";
        
        // End-to-End Latency
        oss << "End-to-End Latency (ns):\n";
        oss << "  p50=" << setprecision(0) << end_to_end_stats.p50 
            << " p95=" << end_to_end_stats.p95 
            << " p99=" << end_to_end_stats.p99
            << " mean=" << setprecision(2) << end_to_end_stats.mean << "\n";
        
        return oss.str();
    }
};

// ============================================================================
// SPSC BENCHMARK (1 Producer, 1 Consumer)
// ============================================================================

class SPSCBenchmark {
private:
    queue<BenchmarkEvent> q;
    mutex q_mutex;
    condition_variable q_cv;
    atomic<bool> done{false};
    LatencyMetrics metrics;
    uint64_t start_time_ns;
    
public:
    SPSCBenchmark() : start_time_ns(0) {
        metrics.reserve(10'000'000);
    }
    
    void producer_thread(size_t total_events) {
        start_time_ns = Clock::now_ns();
        
        for (uint32_t i = 0; i < total_events; ++i) {
            BenchmarkEvent evt(i, 64);
            
            {
                lock_guard<mutex> lock(q_mutex);
                q.push(evt);
            }
            q_cv.notify_one();
        }
        
        done = true;
        q_cv.notify_one();
    }
    
    void consumer_thread() {
        while (true) {
            unique_lock<mutex> lock(q_mutex);
            q_cv.wait(lock, [this] { return !q.empty() || done; });
            
            if (q.empty() && done) break;
            
            if (!q.empty()) {
                BenchmarkEvent evt = q.front();
                q.pop();
                lock.unlock();
                
                // Record dequeue and processing done timestamps
                evt.dequeue_ts = Clock::now_ns();
                // Simulate minimal processing
                evt.process_done_ts = Clock::now_ns();
                
                metrics.add_latencies(evt);
            }
        }
    }
    
    BenchmarkResult run(size_t total_events) {
        metrics.queue_wait.clear();
        metrics.processing.clear();
        metrics.end_to_end.clear();
        done = false;
        
        thread producer([this, total_events] { producer_thread(total_events); });
        thread consumer([this] { consumer_thread(); });
        
        producer.join();
        consumer.join();
        
        uint64_t end_time_ns = Clock::now_ns();
        double duration_sec = (end_time_ns - start_time_ns) / 1e9;
        
        BenchmarkResult result;
        result.scenario = Scenario::SPSC;
        result.total_events = total_events;
        result.processed_events = metrics.queue_wait.size();
        result.duration_sec = duration_sec;
        result.num_producers = 1;
        result.num_consumers = 1;
        
        if (duration_sec > 0) {
            result.throughput = total_events / duration_sec;
        }
        
        result.queue_wait_stats = metrics.calculate_stats(metrics.queue_wait);
        result.processing_stats = metrics.calculate_stats(metrics.processing);
        result.end_to_end_stats = metrics.calculate_stats(metrics.end_to_end);
        
        return result;
    }
};

// ============================================================================
// MPSC BENCHMARK (N Producers, 1 Consumer)
// ============================================================================

class MPSCBenchmark {
private:
    queue<BenchmarkEvent> q;
    mutex q_mutex;
    condition_variable q_cv;
    atomic<bool> done{false};
    atomic<size_t> events_produced{0};
    LatencyMetrics metrics;
    uint64_t start_time_ns;
    
public:
    MPSCBenchmark() : start_time_ns(0) {
        metrics.reserve(10'000'000);
    }
    
    void producer_thread(uint32_t thread_id, size_t events_per_producer, 
                        size_t num_producers) {
        size_t local_count = 0;
        
        for (uint32_t i = 0; i < events_per_producer; ++i) {
            uint32_t event_id = thread_id * events_per_producer + i;
            BenchmarkEvent evt(event_id, 64);
            
            {
                lock_guard<mutex> lock(q_mutex);
                q.push(evt);
                local_count++;
            }
            q_cv.notify_one();
        }
        
        events_produced += local_count;
        
        // Last producer sets done flag
        if (thread_id == num_producers - 1) {
            {
                lock_guard<mutex> lock(q_mutex);
                done = true;
            }
            q_cv.notify_one();
        }
    }
    
    void consumer_thread() {
        while (true) {
            unique_lock<mutex> lock(q_mutex);
            q_cv.wait(lock, [this] { return !q.empty() || done; });
            
            if (q.empty() && done) break;
            
            if (!q.empty()) {
                BenchmarkEvent evt = q.front();
                q.pop();
                lock.unlock();
                
                evt.dequeue_ts = Clock::now_ns();
                evt.process_done_ts = Clock::now_ns();
                
                metrics.add_latencies(evt);
            }
        }
    }
    
    BenchmarkResult run(size_t total_events, int num_producers = 0) {
        if (num_producers == 0) {
            num_producers = thread::hardware_concurrency();
        }
        
        metrics.queue_wait.clear();
        metrics.processing.clear();
        metrics.end_to_end.clear();
        events_produced = 0;
        done = false;
        
        size_t events_per_producer = total_events / num_producers;
        
        start_time_ns = Clock::now_ns();
        
        vector<thread> producers;
        for (int i = 0; i < num_producers; ++i) {
            producers.emplace_back([this, i, events_per_producer, num_producers] {
                producer_thread(i, events_per_producer, num_producers);
            });
        }
        
        thread consumer([this] { consumer_thread(); });
        
        for (auto& t : producers) t.join();
        consumer.join();
        
        uint64_t end_time_ns = Clock::now_ns();
        double duration_sec = (end_time_ns - start_time_ns) / 1e9;
        
        BenchmarkResult result;
        result.scenario = Scenario::MPSC;
        result.total_events = total_events;
        result.processed_events = metrics.queue_wait.size();
        result.duration_sec = duration_sec;
        result.num_producers = num_producers;
        result.num_consumers = 1;
        
        if (duration_sec > 0) {
            result.throughput = total_events / duration_sec;
        }
        
        result.queue_wait_stats = metrics.calculate_stats(metrics.queue_wait);
        result.processing_stats = metrics.calculate_stats(metrics.processing);
        result.end_to_end_stats = metrics.calculate_stats(metrics.end_to_end);
        
        return result;
    }
};

// ============================================================================
// MPMC BENCHMARK (N Producers, M Consumers)
// ============================================================================

class MPMCBenchmark {
private:
    queue<BenchmarkEvent> q;
    mutex q_mutex;
    condition_variable q_cv;
    atomic<size_t> events_produced{0};
    atomic<size_t> events_consumed{0};
    atomic<bool> producers_done{false};
    LatencyMetrics metrics;
    mutex metrics_mutex;
    uint64_t start_time_ns;
    
public:
    MPMCBenchmark() : start_time_ns(0) {
        metrics.reserve(10'000'000);
    }
    
    void producer_thread(uint32_t thread_id, size_t events_per_producer,
                        size_t num_producers, size_t total_events) {
        for (uint32_t i = 0; i < events_per_producer; ++i) {
            uint32_t event_id = thread_id * events_per_producer + i;
            BenchmarkEvent evt(event_id, 64);
            
            {
                lock_guard<mutex> lock(q_mutex);
                q.push(evt);
            }
            events_produced++;
            q_cv.notify_one();
        }
        
        // Wait for all producers to finish
        while (events_produced < total_events) {
            this_thread::sleep_for(chrono::microseconds(100));
        }
        
        // Last producer sets done flag
        if (thread_id == 0) {
            {
                lock_guard<mutex> lock(q_mutex);
                producers_done = true;
            }
            q_cv.notify_all();
        }
    }
    
    void consumer_thread() {
        while (true) {
            unique_lock<mutex> lock(q_mutex);
            q_cv.wait(lock, [this] { return !q.empty() || producers_done; });
            
            if (q.empty() && producers_done) {
                break;
            }
            
            if (!q.empty()) {
                BenchmarkEvent evt = q.front();
                q.pop();
                lock.unlock();
                
                evt.dequeue_ts = Clock::now_ns();
                evt.process_done_ts = Clock::now_ns();
                
                {
                    lock_guard<mutex> lat_lock(metrics_mutex);
                    metrics.add_latencies(evt);
                }
                
                events_consumed++;
            }
        }
    }
    
    BenchmarkResult run(size_t total_events, int num_producers = 0, int num_consumers = 0) {
        if (num_producers == 0) {
            num_producers = thread::hardware_concurrency();
        }
        if (num_consumers == 0) {
            num_consumers = thread::hardware_concurrency();
        }
        
        metrics.queue_wait.clear();
        metrics.processing.clear();
        metrics.end_to_end.clear();
        events_produced = 0;
        events_consumed = 0;
        producers_done = false;
        
        size_t events_per_producer = total_events / num_producers;
        
        start_time_ns = Clock::now_ns();
        
        vector<thread> producers;
        for (int i = 0; i < num_producers; ++i) {
            producers.emplace_back([this, i, events_per_producer, num_producers, total_events] {
                producer_thread(i, events_per_producer, num_producers, total_events);
            });
        }
        
        vector<thread> consumers;
        for (int i = 0; i < num_consumers; ++i) {
            consumers.emplace_back([this] {
                consumer_thread();
            });
        }
        
        for (auto& t : producers) t.join();
        for (auto& t : consumers) t.join();
        
        uint64_t end_time_ns = Clock::now_ns();
        double duration_sec = (end_time_ns - start_time_ns) / 1e9;
        
        BenchmarkResult result;
        result.scenario = Scenario::MPMC;
        result.total_events = total_events;
        result.processed_events = metrics.queue_wait.size();
        result.duration_sec = duration_sec;
        result.num_producers = num_producers;
        result.num_consumers = num_consumers;
        
        if (duration_sec > 0) {
            result.throughput = total_events / duration_sec;
        }
        
        result.queue_wait_stats = metrics.calculate_stats(metrics.queue_wait);
        result.processing_stats = metrics.calculate_stats(metrics.processing);
        result.end_to_end_stats = metrics.calculate_stats(metrics.end_to_end);
        
        return result;
    }
};

// ============================================================================
// MAIN BENCHMARK RUNNER
// ============================================================================

int main(int argc, char* argv[]) {
    cout << "\n";
    cout << "========================================================================\n";
    cout << "  EVENTSTREAM CORE - DAY 32 PERFORMANCE BENCHMARK (FIXED CLOCK)\n";
    cout << "  Improvements: steady_clock + split latency measurement\n";
    cout << "========================================================================\n";
    
    const size_t TOTAL_EVENTS = 5'000'000;
    
    cout << "\nConfiguration:\n";
    cout << "  Total events: " << TOTAL_EVENTS << "\n";
    cout << "  Hardware concurrency: " << thread::hardware_concurrency() << "\n";
    cout << "  Clock: steady_clock (monotonic, NTP-independent)\n";
    cout << "\nRunning benchmarks...\n";
    
    vector<BenchmarkResult> results;
    
    // ========== SPSC BENCHMARK ==========
    cout << "\n[1/3] Running SPSC (1 producer, 1 consumer)...\n";
    cout.flush();
    SPSCBenchmark spsc_bench;
    auto spsc_result = spsc_bench.run(TOTAL_EVENTS);
    results.push_back(spsc_result);
    cout << spsc_result.to_string();
    
    // ========== MPSC BENCHMARK ==========
    cout << "\n[2/3] Running MPSC (N producers, 1 consumer)...\n";
    cout.flush();
    MPSCBenchmark mpsc_bench;
    auto mpsc_result = mpsc_bench.run(TOTAL_EVENTS);
    results.push_back(mpsc_result);
    cout << mpsc_result.to_string();
    
    // ========== MPMC BENCHMARK ==========
    cout << "\n[3/3] Running MPMC (N producers, M consumers)...\n";
    cout.flush();
    MPMCBenchmark mpmc_bench;
    auto mpmc_result = mpmc_bench.run(TOTAL_EVENTS);
    results.push_back(mpmc_result);
    cout << mpmc_result.to_string();
    
    // ========== WRITE RESULTS TO FILE ==========
    cout << "\nWriting results to benchmarks/results/day32_clock_fixed.txt...\n";
    
    system("mkdir -p benchmarks/results");
    
    ofstream outfile("benchmarks/results/day32_clock_fixed.txt");
    if (outfile.is_open()) {
        outfile << "========================================================================\n";
        outfile << "  EVENTSTREAM CORE - DAY 32 PERFORMANCE BENCHMARK\n";
        outfile << "  Clock Fixed: steady_clock (monotonic)\n";
        outfile << "  Split Latency: queue_wait + processing + end_to_end\n";
        outfile << "========================================================================\n";
        outfile << "Total Events per Scenario: " << TOTAL_EVENTS << "\n";
        outfile << "Hardware Concurrency: " << thread::hardware_concurrency() << "\n";
        outfile << "========================================================================\n";
        
        for (const auto& result : results) {
            outfile << result.to_string();
            outfile << "----------------------------------------\n";
        }
        
        outfile << "\nKey Observations:\n";
        outfile << "  1. Queue Wait Latency: Time from ingest to dequeue\n";
        outfile << "  2. Processing Latency: Time from dequeue to done\n";
        outfile << "  3. End-to-End Latency: Total time from ingest to done\n";
        outfile << "\n  Expected with steady_clock:\n";
        outfile << "  - SPSC queue_wait: < 10µs (microseconds, not milliseconds)\n";
        outfile << "  - MPSC bottleneck: Single consumer blocks on lock contention\n";
        outfile << "  - MPMC: Better queue_wait with multiple consumers\n";
        
        outfile.close();
        cout << "✓ Results saved to benchmarks/results/day32_clock_fixed.txt\n";
    } else {
        cerr << "✗ Failed to write results file\n";
    }
    
    cout << "\n========================================================================\n";
    cout << "  ✓ DAY 32 BENCHMARK COMPLETED\n";
    cout << "  Next: Perf profiling to identify real bottlenecks\n";
    cout << "========================================================================\n\n";
    
    return 0;
}
