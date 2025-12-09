// ============================================================================
// COMPREHENSIVE EVENTSTREAM BENCHMARK SUITE
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
#include <cstring>
#include <map>
#include <set>
#include <queue>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <unistd.h>

#include "event/Event.hpp"
#include "event/EventBus.hpp"
#include "eventprocessor/event_processor.hpp"
#include "storage_engine/storage_engine.hpp"
#include "utils/thread_pool.hpp"

using namespace std;
using namespace std::chrono;
using namespace EventStream;

// ============================================================================
// BENCHMARK RESULT STRUCTURE
// ============================================================================

struct BenchmarkResult {
    double throughput_eps;
    double latency_avg_us;
    double latency_p50_us;
    double latency_p99_us;
    double latency_max_us;
    size_t total_events;
    size_t successful_events;
    size_t failed_events;
    double duration_sec;
    
    void print() const {
        cout << "\n--- Results ---" << endl;
        cout << fixed << setprecision(2);
        cout << "Total events: " << total_events << endl;
        cout << "Successful: " << successful_events << " | Failed: " << failed_events << endl;
        if (total_events > 0) {
            cout << "Success rate: " << (successful_events * 100.0 / total_events) << "%" << endl;
        }
        cout << "Duration: " << duration_sec << " sec" << endl;
        cout << "Throughput: " << throughput_eps << " events/sec" << endl;
        if (latency_avg_us > 0) {
            cout << "Latency avg: " << latency_avg_us << " us" << endl;
            cout << "Latency p50: " << latency_p50_us << " us" << endl;
            cout << "Latency p99: " << latency_p99_us << " us" << endl;
            cout << "Latency max: " << latency_max_us << " us" << endl;
        }
    }
};

// ============================================================================
// BENCHMARK 1: EventBus Throughput & Latency
// ============================================================================

class EventBusBenchmark {
private:
    EventBus* bus;
    atomic<size_t> events_sent{0};
    atomic<size_t> events_received{0};
    atomic<size_t> events_failed{0};
    mutex latency_mutex;
    vector<int64_t> latencies_ns;
    
public:
    EventBusBenchmark(EventBus* eb) : bus(eb) {
        latencies_ns.reserve(1000000);
    }
    
    BenchmarkResult runThroughputTest(int num_publishers, int num_subscribers, 
                                       int events_per_publisher) {
        cout << "\n=== EventBus Throughput Test ===" << endl;
        cout << "Publishers: " << num_publishers << endl;
        cout << "Subscribers: " << num_subscribers << endl;
        cout << "Events per publisher: " << events_per_publisher << endl;
        
        events_sent = 0;
        events_received = 0;
        events_failed = 0;
        latencies_ns.clear();
        
        // Register subscribers
        for (int i = 0; i < num_subscribers; i++) {
            bus->subscribe([this](const Event& evt) {
                events_received++;
                
                auto now_ns = steady_clock::now().time_since_epoch().count();
                int64_t latency = now_ns - evt.header.timestamp;
                
                lock_guard<mutex> lock(latency_mutex);
                if (latencies_ns.size() < 100000) {
                    latencies_ns.push_back(latency);
                }
            });
        }
        
        auto start = steady_clock::now();
        
        vector<thread> pub_threads;
        for (int i = 0; i < num_publishers; i++) {
            pub_threads.emplace_back([this, events_per_publisher, i]() {
                for (int j = 0; j < events_per_publisher; j++) {
                    try {
                        Event evt;
                        evt.header.id = i * 1000000ULL + j;
                        evt.header.sourceType = EventSourceType::INTERNAL;
                        evt.header.timestamp = steady_clock::now().time_since_epoch().count();
                        evt.topic = "benchmark_topic";
                        
                        string payload_str = "pub_" + to_string(i) + "_msg_" + to_string(j);
                        evt.body.assign(payload_str.begin(), payload_str.end());
                    
                        
                        bus->publishEvent(evt);
                        events_sent++;
                    } catch (const exception& e) {
                        events_failed++;
                    }
                }
            });
        }
        
        for (auto& t : pub_threads) {
            t.join();
        }
        
        auto end = steady_clock::now();
        double elapsed = duration_cast<milliseconds>(end - start).count() / 1000.0;
        
        this_thread::sleep_for(milliseconds(200));
        
        BenchmarkResult result;
        result.total_events = events_sent + events_failed;
        result.successful_events = events_sent;
        result.failed_events = events_failed;
        result.duration_sec = elapsed;
        result.throughput_eps = events_sent / elapsed;
        
        if (!latencies_ns.empty()) {
            sort(latencies_ns.begin(), latencies_ns.end());
            result.latency_avg_us = accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0) 
                                    / latencies_ns.size() / 1000.0;
            result.latency_p50_us = latencies_ns[latencies_ns.size() / 2] / 1000.0;
            result.latency_p99_us = latencies_ns[(latencies_ns.size() * 99) / 100] / 1000.0;
            result.latency_max_us = latencies_ns.back() / 1000.0;
        } else {
            result.latency_avg_us = result.latency_p50_us = result.latency_p99_us = result.latency_max_us = 0;
        }
        
        result.print();
        cout << "\nEvents received by subscribers: " << events_received << endl;
        if (num_subscribers > 0) {
            cout << "Per-subscriber delivery: " << (events_received / (double)num_subscribers) << endl;
        }
        
        return result;
    }
};

// ============================================================================
// BENCHMARK 2: TCP Load Test (Client Simulation)
// ============================================================================

class TCPLoadGenerator {
private:
    string server_ip;
    int server_port;
    atomic<size_t> messages_sent{0};
    atomic<size_t> errors{0};
    
public:
    TCPLoadGenerator(const string& ip, int port) 
        : server_ip(ip), server_port(port) {}
    
    bool sendFrame(int sock, const string& topic, const string& payload) {
        uint32_t topic_len = htonl(topic.size());
        uint32_t payload_len = htonl(payload.size());
        
    #ifdef _WIN32 
        if (send(sock, (const char*)&topic_len, 4, 0) != 4) return false;
        if (send(sock, topic.c_str(), (int)topic.size(), 0) != (int)topic.size()) return false;
        if (send(sock, (const char*)&payload_len, 4, 0) != 4) return false;
        if (send(sock, payload.c_str(), (int)payload.size(), 0) != (int)payload.size()) return false;


    #else 
        if (send(sock, &topic_len, 4, 0) != 4) return false;
        if (send(sock, topic.c_str(), topic.size(), 0) != (ssize_t)topic.size()) return false;
        if (send(sock, &payload_len, 4, 0) != 4) return false;
        if (send(sock, payload.c_str(), payload.size(), 0) != (ssize_t)payload.size()) return false;
    #endif
        return true;
    }
    
    BenchmarkResult runLoadTest(int num_clients, int messages_per_client) {
        cout << "\n=== TCP Load Test ===" << endl;
        cout << "Clients: " << num_clients << endl;
        cout << "Messages per client: " << messages_per_client << endl;
        cout << "Target: " << server_ip << ":" << server_port << endl;
        
        messages_sent = 0;
        errors = 0;
        
        auto start = steady_clock::now();
        
        vector<thread> client_threads;
        for (int i = 0; i < num_clients; i++) {
            client_threads.emplace_back([this, messages_per_client, i]() {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) {
                    errors++;
                    return;
                }
                
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(server_port);
                inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr);
                
                if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
                    errors++;
                    close(sock);
                    return;
                }
                
                for (int j = 0; j < messages_per_client; j++) {
                    string topic = "benchmark_topic";
                    string payload = "client_" + to_string(i) + "_msg_" + to_string(j);
                    
                    if (sendFrame(sock, topic, payload)) {
                        messages_sent++;
                    } else {
                        errors++;
                    }
                    
                    this_thread::sleep_for(microseconds(10));
                }
                
                close(sock);
            });
        }
        
        for (auto& t : client_threads) {
            t.join();
        }
        
        auto end = steady_clock::now();
        double elapsed = duration_cast<milliseconds>(end - start).count() / 1000.0;
        
        BenchmarkResult result;
        result.total_events = messages_sent + errors;
        result.successful_events = messages_sent;
        result.failed_events = errors;
        result.duration_sec = elapsed;
        result.throughput_eps = (elapsed > 0) ? (messages_sent / elapsed) : 0;
        result.latency_avg_us = 0;
        result.latency_p50_us = 0;
        result.latency_p99_us = 0;
        result.latency_max_us = 0;
        
        result.print();
        
        size_t total_attempts = messages_sent + errors;
        if (total_attempts > 0) {
            cout << "\nConnection reliability: " << ((total_attempts - errors) * 100.0 / total_attempts) << "%" << endl;
        }
        
        return result;
    }
};


// ============================================================================
// BENCHMARK 4: Storage Engine Benchmark
// ============================================================================

class StorageBenchmark {
private:
    StorageEngine* storage;
    atomic<size_t> writes_completed{0};
    atomic<size_t> writes_failed{0};
    mutex latency_mutex;
    vector<int64_t> write_latencies_ns;
    
public:
    StorageBenchmark(StorageEngine* se) : storage(se) {
        write_latencies_ns.reserve(100000);
    }
    
    BenchmarkResult runWriteBenchmark(int num_writers, int writes_per_writer) {
        cout << "\n=== Storage Write Benchmark ===" << endl;
        cout << "Writers: " << num_writers << endl;
        cout << "Writes per writer: " << writes_per_writer << endl;
        
        writes_completed = 0;
        writes_failed = 0;
        write_latencies_ns.clear();
        
        auto start = steady_clock::now();
        
        vector<thread> writer_threads;
        for (int i = 0; i < num_writers; i++) {
            writer_threads.emplace_back([this, writes_per_writer, i]() {
                for (int j = 0; j < writes_per_writer; j++) {
                    try {
                        auto write_start = steady_clock::now().time_since_epoch().count();
                        
                        Event evt;
                        evt.header.id = i * 1000000ULL + j;
                        evt.header.sourceType = EventSourceType::INTERNAL;
                        evt.header.timestamp = steady_clock::now().time_since_epoch().count();
                        evt.topic = "storage_benchmark";
                        
                        string payload = "writer_" + to_string(i) + "_record_" + to_string(j);
                        evt.body.assign(payload.begin(), payload.end());
                  
                        
                        storage->storeEvent(evt);
                        
                        auto write_end = steady_clock::now().time_since_epoch().count();
                        int64_t latency = write_end - write_start;
                        
                        lock_guard<mutex> lock(latency_mutex);
                        if (write_latencies_ns.size() < 100000) {
                            write_latencies_ns.push_back(latency);
                        }
                        
                        writes_completed++;
                    } catch (const exception& e) {
                        writes_failed++;
                    }
                }
            });
        }
        
        for (auto& t : writer_threads) {
            t.join();
        }
        
        auto end = steady_clock::now();
        double elapsed = duration_cast<milliseconds>(end - start).count() / 1000.0;
        
        BenchmarkResult result;
        result.total_events = writes_completed + writes_failed;
        result.successful_events = writes_completed;
        result.failed_events = writes_failed;
        result.duration_sec = elapsed;
        result.throughput_eps = writes_completed / elapsed;
        
        if (!write_latencies_ns.empty()) {
            sort(write_latencies_ns.begin(), write_latencies_ns.end());
            result.latency_avg_us = accumulate(write_latencies_ns.begin(), write_latencies_ns.end(), 0.0) 
                                    / write_latencies_ns.size() / 1000.0;
            result.latency_p50_us = write_latencies_ns[write_latencies_ns.size() / 2] / 1000.0;
            result.latency_p99_us = write_latencies_ns[(write_latencies_ns.size() * 99) / 100] / 1000.0;
            result.latency_max_us = write_latencies_ns.back() / 1000.0;
        }
        
        result.print();
        cout << "\nWrite success rate: " << (writes_completed * 100.0 / result.total_events) << "%" << endl;
        
        return result;
    }
};

// ============================================================================
// PROFILER HELPER
// ============================================================================

class ProfilerHelper {
public:
    static void printSystemInfo() {
        cout << "\n=== System Information ===" << endl;
        cout << "Hardware threads: " << thread::hardware_concurrency() << endl;
        system("cat /proc/cpuinfo | grep 'model name' | head -1");
        system("free -h | grep Mem");
    }
    
    static void suggestProfilingCommands() {
        cout << "\n=== Suggested Profiling Commands ===" << endl;
        cout << "CPU profiling:" << endl;
        cout << "  perf record -g ./benchmark --all" << endl;
        cout << "  perf report" << endl;
        cout << "\nMemory profiling:" << endl;
        cout << "  valgrind --tool=massif ./benchmark --all" << endl;
        cout << "  ms_print massif.out.*" << endl;
    }
};

// ============================================================================
// MAIN
// ============================================================================

void printBanner() {
    cout << "\n╔════════════════════════════════════════╗" << endl;
    cout << "║   EVENTSTREAM COMPREHENSIVE BENCHMARK  ║" << endl;
    cout << "║      Component & Integration Tests     ║" << endl;
    cout << "╚════════════════════════════════════════╝" << endl;
}

int main(int argc, char* argv[]) {
    printBanner();
    ProfilerHelper::printSystemInfo();
    
    bool run_eventbus = true;
    bool run_tcp = false;  // Requires server running
    bool run_processor = true;
    bool run_storage = true;
    
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--eventbus-only") {
            run_tcp = run_processor = run_storage = false;
        } else if (arg == "--tcp-only") {
            run_eventbus = run_processor = run_storage = false;
            run_tcp = true;
        } else if (arg == "--processor-only") {
            run_eventbus = run_tcp = run_storage = false;
            run_processor = true;
        } else if (arg == "--storage-only") {
            run_eventbus = run_tcp = run_processor = false;
        } else if (arg == "--all") {
            run_eventbus = run_tcp = run_processor = run_storage = true;
        } else if (arg == "--help") {
            cout << "\nUsage: ./benchmark [options]" << endl;
            cout << "Options:" << endl;
            cout << "  --eventbus-only    EventBus throughput test only" << endl;
            cout << "  --tcp-only         TCP load test (requires server on 9090)" << endl;
            cout << "  --processor-only   Event processor test only" << endl;
            cout << "  --storage-only     Storage write test only" << endl;
            cout << "  --all              Run all benchmarks" << endl;
            cout << "  --help             Show this message" << endl;
            return 0;
        }
    }
    
    // Initialize components
    EventBus bus;
    StorageEngine storage("benchmark/benchmark_output.txt");
    ThreadPool pool(4);
    EventProcessor processor(bus, storage, &pool);
    
    // Benchmark 1: EventBus
    if (run_eventbus) {
        cout << "\n\n[1/4] Running EventBus Benchmark..." << endl;
        EventBusBenchmark eb_bench(&bus);
        eb_bench.runThroughputTest(1, 1, 10000);
        eb_bench.runThroughputTest(4, 8, 50000);
    }
    
    // Benchmark 2: TCP Load
    if (run_tcp) {
        cout << "\n\n[2/4] Running TCP Load Test..." << endl;
        cout << "NOTE: Make sure your TCP server is running on port 9000" << endl;
        cout << "Press Enter to continue, or Ctrl+C to skip...";
        cin.get();
        
        TCPLoadGenerator tcp_gen("127.0.0.1", 9000);
        tcp_gen.runLoadTest(5, 200);
        tcp_gen.runLoadTest(10, 500);
    }
    
    
    // Benchmark 3: Storage
    if (run_storage) {
        cout << "\n\n[4/4] Running Storage Benchmark..." << endl;
        StorageBenchmark storage_bench(&storage);
        storage_bench.runWriteBenchmark(1, 5000);
        storage_bench.runWriteBenchmark(4, 5000);
    }
    
    ProfilerHelper::suggestProfilingCommands();
    
    cout << "\n\n╔════════════════════════════════════════╗" << endl;
    cout << "║      BENCHMARK COMPLETE                ║" << endl;
    cout << "╚════════════════════════════════════════╝" << endl;
    
    return 0;
}