// ============================================================================
// DAY 32 - SPSC ONLY BENCHMARK FOR PROFILING
// ============================================================================
// This simplified version runs only SPSC to focus on profiling
// Usage: perf record -F 99 -g ./benchmark_spsc
//        perf report

#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <queue>
#include <mutex>
#include <condition_variable>

using namespace std;
using namespace std::chrono;

class Clock {
public:
    static inline uint64_t now_ns() {
        auto now = steady_clock::now();
        return duration_cast<nanoseconds>(now.time_since_epoch()).count();
    }
};

struct BenchmarkEvent {
    uint32_t id;
    uint64_t ingest_ts;
    uint64_t dequeue_ts;
    uint64_t process_done_ts;
    uint32_t payload_size;
    
    BenchmarkEvent() 
        : id(0), ingest_ts(0), dequeue_ts(0), process_done_ts(0), payload_size(0) {}
    
    BenchmarkEvent(uint32_t id_, uint32_t payload_sz)
        : id(id_), ingest_ts(Clock::now_ns()), 
          dequeue_ts(0), process_done_ts(0), payload_size(payload_sz) {}
    
    uint64_t queue_wait_latency_ns() const {
        return dequeue_ts - ingest_ts;
    }
};

class SPSCBenchmark {
private:
    queue<BenchmarkEvent> q;
    mutex q_mutex;
    condition_variable q_cv;
    atomic<bool> done{false};
    vector<uint64_t> latencies;
    uint64_t start_time_ns;
    
public:
    SPSCBenchmark() : start_time_ns(0) {
        latencies.reserve(10'000'000);
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
                
                evt.dequeue_ts = Clock::now_ns();
                evt.process_done_ts = Clock::now_ns();
                
                latencies.push_back(evt.queue_wait_latency_ns());
            }
        }
    }
    
    void run(size_t total_events) {
        latencies.clear();
        done = false;
        
        cout << "Running SPSC benchmark with " << total_events << " events...\n";
        cout.flush();
        
        thread producer([this, total_events] { producer_thread(total_events); });
        thread consumer([this] { consumer_thread(); });
        
        producer.join();
        consumer.join();
        
        uint64_t end_time_ns = Clock::now_ns();
        double duration_sec = (end_time_ns - start_time_ns) / 1e9;
        
        cout << "\nResults:\n";
        cout << "  Total events: " << total_events << "\n";
        cout << "  Processed: " << latencies.size() << "\n";
        cout << "  Duration: " << duration_sec << " sec\n";
        cout << "  Throughput: " << (total_events / duration_sec / 1e6) << "M events/sec\n";
        
        if (!latencies.empty()) {
            sort(latencies.begin(), latencies.end());
            cout << "  Queue wait latency (ns):\n";
            cout << "    p50=" << latencies[latencies.size() * 50 / 100] << "\n";
            cout << "    p95=" << latencies[latencies.size() * 95 / 100] << "\n";
            cout << "    p99=" << latencies[latencies.size() * 99 / 100] << "\n";
        }
    }
};

int main() {
    cout << "===============================================================\n";
    cout << "  DAY 32 - SPSC PROFILING BENCHMARK\n";
    cout << "  For use with: perf record -F 99 -g ./benchmark_spsc\n";
    cout << "===============================================================\n\n";
    
    const size_t TOTAL_EVENTS = 5'000'000;
    
    SPSCBenchmark bench;
    bench.run(TOTAL_EVENTS);
    
    cout << "\n===============================================================\n";
    cout << "  ✓ Benchmark complete - Ready for profiling analysis\n";
    cout << "===============================================================\n\n";
    
    return 0;
}
