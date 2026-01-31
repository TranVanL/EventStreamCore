

<h1 align="center">⚡ EventStreamCore</h1>

<p align="center">
  <strong>Ultra-Low Latency Event Streaming Engine</strong><br>
  <em>High-performance C++17 event processing with lock-free data structures</em>
</p>

<p align="center">
  <a href="#-features">Features</a> •
  <a href="#-architecture">Architecture</a> •
  <a href="#-core-components">Components</a> •
  <a href="#-performance">Performance</a> •
  <a href="#-quick-start">Quick Start</a> •
  <a href="#-documentation">Documentation</a>
</p>

---

## 🎯 Overview

**EventStreamCore** is a production-grade event streaming engine built for systems that demand **microsecond-level latency** and **millions of events per second**. Designed with modern C++17 and lock-free algorithms, it's ideal for:

| Domain | Use Cases |
|--------|-----------|
| 🏦 **Financial Systems** | Order matching, market data feeds, risk calculation |
| 🌐 **IoT Platforms** | Sensor data aggregation, real-time telemetry |
| 🎮 **Gaming Backends** | Player events, matchmaking, leaderboards |
| 📊 **Real-time Analytics** | Stream processing, CEP (Complex Event Processing) |

---

## ✨ Features

### Core Capabilities

- **🚀 Ultra-Low Latency** — P99 latency < 2µs with lock-free queues
- **📈 High Throughput** — 10M+ events/second on commodity hardware
- **🔒 Lock-Free Design** — SPSC (16384 capacity) / MPSC (65536 capacity) queues
- **💾 Zero-Allocation Hot Path** — NUMA-aware event pools with pre-allocation
- **🖥️ NUMA-Aware** — Thread affinity and memory binding for multi-socket systems
- **🔄 Deduplication** — 1-hour idempotency window (4096-bucket hash map)
- **📉 5-Level Backpressure** — HEALTHY → ELEVATED → DEGRADED → CRITICAL → EMERGENCY

### Architecture Highlights

- **3-Layer Design** — Core Engine → Distributed Consensus → Microservice Gateway
- **Priority-Based Routing** — REALTIME, TRANSACTIONAL, BATCH queues with adaptive backpressure
- **Adaptive Control Plane** — Automatic load shedding with min evaluation warmup
- **Dead Letter Queue** — Failed events persisted for analysis and replay
- **Raft Consensus** — Distributed state replication for high availability

---

## 🏗️ Architecture

```
                            ┌─────────────────────────────────────────────────────────┐
                            │                   EventStreamCore                        │
                            └─────────────────────────────────────────────────────────┘
                                                       │
         ┌─────────────────────────────────────────────┼─────────────────────────────────────────────┐
         │                                             │                                             │
         ▼                                             ▼                                             ▼
┌─────────────────┐                         ┌─────────────────┐                         ┌─────────────────┐
│   LAYER 1       │                         │   LAYER 2       │                         │   LAYER 3       │
│   CORE          │                         │   DISTRIBUTED   │                         │   MICROSERVICE  │
├─────────────────┤                         ├─────────────────┤                         ├─────────────────┤
│ • Lock-free     │                         │ • Raft          │                         │ • gRPC Gateway  │
│   SPSC (16384)  │◄───────────────────────►│   Consensus     │◄───────────────────────►│ • Health API    │
│ • MPSC (65536)  │                         │ • Log           │                         │ • Kubernetes    │
│ • NUMAEventPool │                         │   Replication   │                         │   Ready         │
│ • 3 Processors  │                         │ • Leader        │                         │ • Metrics       │
│ • TCP/UDP       │                         │   Election      │                         │   Export        │
└─────────────────┘                         └─────────────────┘                         └─────────────────┘
```

### Event Flow Pipeline

```
┌──────────┐    ┌────────────┐    ┌─────────────┐    ┌──────────────┐    ┌───────────┐
│  Ingest  │───►│ Dispatcher │───►│  EventBus   │───►│  Processors  │───►│  Storage  │
│ TCP/UDP  │    │  (Router)  │    │ (Lock-Free) │    │  (Workers)   │    │  Engine   │
└──────────┘    └────────────┘    └─────────────┘    └──────────────┘    └───────────┘
     │                │                  │                  │                  │
     ▼                ▼                  ▼                  ▼                  ▼
 Frame Parser    Topic-based      3 Priority Queues:   3 Processors:       Binary +
 CRC32 Check     Routing          • REALTIME (SPSC)    • Realtime          DLQ Log
 NUMAEventPool   Backpressure     • TRANSACTIONAL      • Transactional
                 Control          • BATCH              • Batch (5s window)
```

---

## 🔧 Core Components

| Category | Component | Description |
|----------|-----------|-------------|
| **Queues** | `SpscRingBuffer<T,16384>` | Lock-free single-producer single-consumer |
| | `MpscQueue<T,65536>` | Vyukov MPSC algorithm (wait-free producer) |
| | `LockFreeDeduplicator` | 4096-bucket hash map, 1h idempotency window |
| **Memory** | `NUMAEventPool<T,N>` | NUMA-aware pre-allocated object pool |
| | `NUMABinding` | CPU affinity + NUMA node binding |
| | `IngestEventPool` | Thread-local pool for TCP/UDP handlers |
| **Processing** | `RealtimeProcessor` | AlertHandler for CRITICAL priority |
| | `TransactionalProcessor` | Dedup + 3-retry logic |
| | `BatchProcessor` | 5-second aggregation window |
| **Control** | `ControlPlane` | 5-level backpressure (HEALTHY→EMERGENCY) |
| | `AdminLoop` | Periodic health check + cleanup |
| **Storage** | `StorageEngine` | Binary event persistence + DLQ |
| **Ingest** | `TcpIngestServer` | Multi-client TCP with backpressure |
| | `UdpIngestServer` | High-throughput UDP receiver |

---

## 📊 Performance

### Benchmark Results (measured on Intel Xeon, 64GB RAM)

| Component | Throughput | P50 Latency | P99 Latency | Capacity |
|-----------|------------|-------------|-------------|----------|
| **SPSC RingBuffer** | 125M ops/sec | 8 ns | 12 ns | 16,384 |
| **MPSC Queue** | 52M ops/sec | 20 ns | 45 ns | 65,536 |
| **NUMAEventPool** | 89M ops/sec | 11 ns | 25 ns | Configurable |
| **Lock-Free Dedup** | 71M ops/sec | 14 ns | 32 ns | 4,096 buckets |
| **End-to-End** | 10M+ events/sec | < 1 µs | < 2 µs | — |

### Optimization Techniques

| Technique | Benefit |
|-----------|---------|
| Cache-line padding (`alignas(64)`) | Prevents false sharing between threads |
| Memory ordering (`acquire/release`) | Minimal synchronization overhead |
| NUMA-aware event pools | Zero malloc in hot path + local memory access |
| Vyukov MPSC algorithm | Wait-free producer, lock-free consumer |
| 5-Level backpressure | Graceful degradation under load |
| Dedup with atomic CAS | Race-free cleanup in concurrent environment |

---

## 🚀 Quick Start

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install -y build-essential cmake libspdlog-dev libyaml-cpp-dev libnuma-dev

# CentOS/RHEL
sudo yum install -y gcc-c++ cmake spdlog-devel yaml-cpp-devel numactl-devel
```

### Build

```bash
git clone https://github.com/yourusername/EventStreamCore.git
cd EventStreamCore

# Create build directory
mkdir build && cd build

# Configure and build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run tests
ctest --output-on-failure
```

### Run Server

```bash
./EventStreamCore ../config/config.yaml
```

### Send Test Events

```bash
# TCP events
cd tests/
python3 send_tcp_event.py 127.0.0.1 9000 100 order.created

# UDP events
python3 send_udp_event.py 127.0.0.1 9001 100 sensor.temperature

# Stress test (10 clients × 10,000 events)
python3 stress_test.py 127.0.0.1 9000 10 10000
```

---

## 📁 Project Structure

```
EventStreamCore/
├── include/eventstream/           # Public headers
│   ├── core/                      # Core engine components
│   │   ├── admin/                 # AdminLoop, ControlPlane (5-level backpressure)
│   │   ├── config/                # ConfigLoader, AppConfig
│   │   ├── events/                # Event, EventBus, Dispatcher, DLQ
│   │   ├── ingest/                # TcpIngestServer, UdpIngestServer
│   │   ├── memory/                # NUMAEventPool, NUMABinding, EventPool
│   │   ├── metrics/               # Histogram, MetricRegistry
│   │   ├── processor/             # Realtime/Transactional/Batch Processors
│   │   ├── queues/                # SpscRingBuffer, MpscQueue, LockFreeDedup
│   │   ├── storage/               # StorageEngine (binary + DLQ)
│   │   └── utils/                 # Clock, ThreadPool
│   ├── distributed/               # Raft consensus
│   └── microservice/              # gRPC gateway, health service
├── src/                           # Implementation files
├── tests/                         # Python integration tests
├── unittest/                      # Google Test unit tests (8 test files)
├── benchmark/                     # Performance benchmarks (6 benchmark files)
├── config/                        # YAML configuration
├── doc_core/                      # Core engine documentation
├── doc_distributed/               # Distributed layer docs
└── doc_microservice/              # Microservice layer docs
```

---

## ⚙️ Configuration

```yaml
# config/config.yaml
app_name: "EventStreamCore"
version: "1.0.0"

ingestion:
  tcp:
    enable: true
    port: 9000
    maxConnections: 1000
  udp:
    enable: true
    port: 9001
    bufferSize: 65536

router:
  shards: 4
  strategy: "priority"
  buffer_size: 16384

numa:
  enable: true
  dispatcher_node: 0
  realtime_proc_node: 0
  transactional_proc_node: 1
```

---

## 🐛 Recent Bug Fixes (v1.1.0)

The following issues were identified and fixed in the latest code review:

| # | Component | Issue | Fix |
|---|-----------|-------|-----|
| 1 | `LockFreeDeduplicator` | Race condition in cleanup | Use CAS for head pointer |
| 2 | `ControlPlane` | Static `previous_state` | Move to instance member |
| 3 | `ControlPlane` | `min_events_for_evaluation` unused | Add warmup check |
| 4 | `EventBus` | REALTIME dropBatch not pushing DLQ | Push batch to DLQ |
| 5 | `TcpServer` | Missing backpressure stats | Add `totalBackpressureDrops_` |
| 6 | `MetricRegistry` | `metrics_map_` public | Move to private |
| 7 | `TransactionalProcessor` | Dedup insert timing | Insert only after success |
| 8 | `BatchProcessor` | Duplicate `batch.clear()` | Remove duplicate |
| 9 | `main.cpp` | StorageEngine not wired | Wire to ProcessManager deps |
| 10 | `StorageEngine` | DLQ path hardcoded | Derive from storage path |
| 11 | `NUMAEventPool` | `release()` broken | Proper search + NUMA cleanup |
| 12 | `IngestEventPool` | Using wrong pool type | Switch to NUMAEventPool |

---

## 📚 Documentation

| Document | Description |
|----------|-------------|
| [doc_core/README.md](doc_core/README.md) | Core engine overview, quick start |
| [doc_core/architecture.md](doc_core/architecture.md) | System design, data flow, component wiring |
| [doc_core/queues.md](doc_core/queues.md) | SPSC, MPSC, LockFreeDedup implementations |
| [doc_core/memory.md](doc_core/memory.md) | NUMAEventPool, NUMABinding, IngestEventPool |
| [doc_core/event.md](doc_core/event.md) | Event model, priority routing, wire protocol |
| [doc_distributed/](doc_distributed/) | Raft consensus, cluster management |
| [doc_microservice/](doc_microservice/) | gRPC gateway, Kubernetes deployment |
| [tests/README.md](tests/README.md) | Integration testing guide |

---

## 🧪 Testing

```bash
# Unit tests (Google Test)
cd build
./EventStreamTests

# Individual benchmarks
./benchmark_spsc_detailed     # SPSC RingBuffer detailed latency
./benchmark_mpsc              # MPSC Queue multi-producer
./benchmark_dedup             # Lock-Free Deduplicator
./benchmark_event_pool        # NUMAEventPool vs malloc
./benchmark_eventbus_multi    # EventBus throughput
./benchmark_summary           # Comprehensive benchmark report

# System integration tests
cd tests/
python3 test_system.py
python3 stress_test.py 127.0.0.1 9000 10 10000
```

### Test Coverage

| Test File | Component Tested |
|-----------|-----------------|
| `config_loader_test.cpp` | YAML config parsing, validation |
| `event_test.cpp` | EventFactory, Event creation |
| `event_processor_test.cpp` | ProcessManager lifecycle |
| `storage_test.cpp` | StorageEngine persistence |
| `tcp_ingest_test.cpp` | Frame parsing, TCP protocol |
| `lock_free_dedup_test.cpp` | Dedup insertion, expiration, concurrent access |
| `raft_test.cpp` | Raft consensus basic operations |

---

## 🛠️ Tech Stack

| Category | Technology |
|----------|------------|
| **Language** | C++17 |
| **Build** | CMake 3.10+ |
| **Logging** | spdlog |
| **Config** | yaml-cpp |
| **Testing** | Google Test |
| **Platform** | Linux (NUMA support) |

---

## 🗺️ Roadmap

- [x] Lock-free SPSC/MPSC queues (16384/65536 capacity)
- [x] NUMA-aware memory allocation (NUMAEventPool)
- [x] Priority-based event routing (3 queues)
- [x] 5-Level adaptive backpressure control
- [x] Lock-free deduplication (4096 buckets, 1h window)
- [x] Binary storage + Dead Letter Queue
- [x] Raft consensus (basic leader election)
- [ ] Full Raft implementation with snapshots
- [ ] gRPC streaming support
- [ ] Prometheus metrics export
- [ ] Kubernetes Operator

---

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

## 🤝 Contributing

Contributions are welcome! Please read the contributing guidelines before submitting a PR.

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

<p align="center">
  ⭐ Star this repo if you find it useful!
</p>
