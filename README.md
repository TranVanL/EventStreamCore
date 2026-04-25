# EventStreamCore

A self-contained event streaming engine in C++17. No Kafka, no RabbitMQ — just a single binary that ingests, routes, processes, and persists events at millions of ops/sec with sub-microsecond latency.

I built this to solve a specific problem: most event systems are either too heavy (you need a whole distributed broker) or too simple (just a `std::queue` with a mutex). This sits in between — a real pipeline with backpressure, deduplication, priority routing, and failure handling, all in one embeddable library.

## Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          INGEST LAYER                                    │
│                                                                          │
│    ┌─────────────┐   ┌─────────────┐                                     │
│    │  TCP Server  │   │  UDP Server  │    (epoll-based, non-blocking)     │
│    │  (multiple   │   │  (recvmmsg   │                                    │
│    │   connections)│   │   batched)   │                                    │
│    └──────┬───────┘   └──────┬───────┘                                    │
│           │                  │                                            │
│           └────────┬─────────┘                                            │
│                    ▼                                                      │
│  ┌─────────────────────────────────────┐                                  │
│  │  MPSC Queue (Vyukov, lock-free)     │  ◄── fan-in: N producers → 1    │
│  │  capacity: 65536                    │      consumer, zero contention   │
│  └──────────────────┬──────────────────┘                                  │
└─────────────────────┼────────────────────────────────────────────────────┘
                      ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                       DISPATCH LAYER                                     │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐     │
│  │                     Dispatcher (single thread)                   │     │
│  │                                                                  │     │
│  │  1. Pop from MPSC inbound queue                                  │     │
│  │  2. Lookup topic → priority via TopicTable                       │     │
│  │  3. Check PipelineState (backpressure adaptive routing)          │     │
│  │  4. Route to target queue in EventBus                            │     │
│  └──────┬───────────────────────┬───────────────────┬───────────────┘     │
│         │                       │                   │                     │
└─────────┼───────────────────────┼───────────────────┼────────────────────┘
          ▼                       ▼                   ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                         EVENT BUS                                        │
│                                                                          │
│  ┌──────────────────┐ ┌────────────────┐ ┌────────────────────┐          │
│  │  Realtime Queue   │ │ Transactional  │ │   Batch Queue      │          │
│  │                   │ │ Queue          │ │                    │          │
│  │  SPSC RingBuffer  │ │ std::deque     │ │ std::deque         │          │
│  │  capacity: 16384  │ │ + mutex + CV   │ │ + mutex + CV       │          │
│  │  (lock-free,      │ │ (ordered       │ │ (window-based      │          │
│  │   zero-copy)      │ │  delivery)     │ │  aggregation)      │          │
│  └────────┬──────────┘ └───────┬────────┘ └─────────┬──────────┘          │
│           │                    │                    │                     │
│  ┌────────┴────────────────────┴────────────────────┴──────────┐         │
│  │              Overflow → Dead Letter Queue (DLQ)             │         │
│  │              (ring buffer, last 1000 events for debug)      │         │
│  └─────────────────────────────────────────────────────────────┘         │
└───────────┼────────────────────┼────────────────────┼────────────────────┘
            ▼                    ▼                    ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                      PROCESSING LAYER                                    │
│                                                                          │
│  ┌──────────────────┐ ┌────────────────┐ ┌────────────────────┐          │
│  │ RealtimeProcessor│ │ Transactional  │ │  BatchProcessor    │          │
│  │                  │ │ Processor      │ │                    │          │
│  │ • Immediate exec │ │ • Ordered exec │ │ • Windowed exec    │          │
│  │ • Alert triggers │ │ • Consistent   │ │ • 5s batch window  │          │
│  │ • Sub-μs path    │ │   state writes │ │ • Aggregation      │          │
│  └────────┬─────────┘ └───────┬────────┘ └─────────┬──────────┘          │
│           │                   │                    │                     │
│           └───────────────────┴────────────────────┘                     │
│                               │                                          │
│                               ▼                                          │
│                      ┌─────────────────┐                                 │
│                      │  StorageEngine  │  (binary persistence)           │
│                      └────────────┬────┘                                 │
│                                   │                                      │
│              ProcessedEventStream (Observer pattern, singleton)          │
│              notify() → subscribers get onEventProcessed() callbacks     │
└───────────────────────────────────┼──────────────────────────────────────┘
                                    │
                    ┌───────────────┴────────────────────┐
                    ▼                                    ▼
       libesccore.so  (C API, extern "C")       gRPC Gateway :50051
       flat structs, no heap, FFI-safe          + Health Service
       Go SDK (cgo) · Python SDK (ctypes)       microservice integration
```

### What makes this fast

The hot path — from ingest to queue insertion — is entirely **lock-free**. Here's how:

- **Ingest fan-in** uses a Vyukov MPSC queue: multiple TCP/UDP threads push concurrently via a single `atomic::exchange` on the tail pointer. No CAS retry loop, no spinlock, O(1) push guaranteed.
- **Realtime queue** is a SPSC ring buffer with `alignas(64)` cache-line separation on head/tail atomics. Power-of-2 capacity means index wrapping is a bitmask (`& (N-1)`) instead of modulo — branchless and fast. This alone benchmarks at **125M ops/s**.
- **Memory allocation** is pre-pooled and NUMA-aware. Events are allocated from a per-node pool at startup — no `malloc` on the hot path.
- **Deduplication** is CAS-based with a configurable TTL window. No locks, no hash map mutex.

The transactional and batch queues intentionally use `std::deque` + `mutex` + `condition_variable` — they don't need lock-free performance, and the mutex gives us ordered delivery guarantees and clean blocking semantics for windowed batching.

### Backpressure

The control plane continuously monitors queue depth, processing latency, and drop rate. When load spikes:

1. **DEGRADED** — batch processing paused, resources shifted to realtime
2. **CRITICAL** — transactions paused, only realtime events processed
3. **OVERLOAD** — low-priority events dropped to DLQ
4. **EMERGENCY** — aggressive shedding, only critical events survive

State transitions use hysteresis (different thresholds for up vs. down) to avoid flapping.

## Performance

Measured on a standard Linux dev box, no kernel tuning:

| Component | Throughput | P99 Latency |
|-----------|-----------|-------------|
| SPSC Ring Buffer | ~125M ops/s | ~12 ns |
| MPSC Queue | ~52M ops/s | ~45 ns |
| Lock-free Dedup | ~71M ops/s | ~32 ns |
| Event Pool alloc | ~89M ops/s | ~25 ns |

## Build & Run

```bash
sudo apt-get install -y build-essential cmake libnuma-dev  # spdlog, yaml-cpp, gtest fetched automatically

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

./EventStreamCore ../config/config.yaml
```

Outputs:
- `EventStreamCore` — standalone server
- `libesccore.so` — C API shared library for cross-language integration

## Integration

```cpp
#include <eventstream/bridge/esccore.h>

esccore_init("config/config.yaml");

esc_event_t evt = {
    .id       = 1,
    .priority = ESC_PRIORITY_HIGH,
    .topic    = "alerts/temperature",
    .body     = sensor_data,
    .body_len = sizeof(sensor_data)
};

esccore_push(&evt);
esccore_shutdown();
```

The C API (`esccore.h`) is the FFI surface — usable from Go, Python, or anything that can `dlopen` a `.so`. SDKs for Go and Python are included under `sdk/`.

## Tests & Benchmarks

```bash
cd build
./EventStreamTests              # unit tests (GTest)
./benchmark_spsc_detailed       # per-component benchmarks
./benchmark_summary             # full summary
```

## Project Structure

```
include/eventstream/
    core/          queues, memory pools, processors, control plane, storage
    bridge/        C API header
    microservice/  gRPC gateway, health checks
src/
    core/          all engine implementation
    bridge/        C API impl (libesccore.so)
    microservice/  gRPC + health service
sdk/
    go/            Go adapter + CLI
    python/        Python bindings
unittest/          Google Test suite
benchmark/         microbenchmarks
config/            YAML config + topic routing rules
```

## Roadmap

- [ ] WebSocket streaming endpoint
- [ ] Prometheus metrics export
- [ ] Kubernetes sidecar deployment
- [ ] Monitoring dashboard


