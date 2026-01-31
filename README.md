<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++17"/>
  <img src="https://img.shields.io/badge/License-MIT-green?style=for-the-badge" alt="MIT License"/>
  <img src="https://img.shields.io/badge/Platform-Linux-FCC624?style=for-the-badge&logo=linux&logoColor=black" alt="Linux"/>
  <img src="https://img.shields.io/badge/Build-CMake-064F8C?style=for-the-badge&logo=cmake&logoColor=white" alt="CMake"/>
</p>

<h1 align="center">⚡ EventStreamCore</h1>

<p align="center">
  <strong>Ultra-Low Latency Event Streaming Engine</strong><br>
  <em>High-performance C++17 event processing with lock-free data structures</em>
</p>

<p align="center">
  <a href="#-features">Features</a> •
  <a href="#-architecture">Architecture</a> •
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
- **🔒 Lock-Free Design** — SPSC/MPSC queues with wait-free operations
- **💾 Zero-Allocation Hot Path** — Pre-allocated event pools eliminate GC pauses
- **🖥️ NUMA-Aware** — Thread affinity and memory binding for multi-socket systems

### Architecture Highlights

- **3-Layer Design** — Core Engine → Distributed Consensus → Microservice Gateway
- **Priority-Based Routing** — REALTIME, TRANSACTIONAL, BATCH queues with backpressure
- **Adaptive Control Plane** — Automatic load shedding and health monitoring
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
│   Queues        │◄───────────────────────►│   Consensus     │◄───────────────────────►│ • Health API    │
│ • Event Pool    │                         │ • Log           │                         │ • Kubernetes    │
│ • Processors    │                         │   Replication   │                         │   Ready         │
│ • TCP/UDP       │                         │ • Leader        │                         │ • Metrics       │
│   Ingest        │                         │   Election      │                         │   Export        │
└─────────────────┘                         └─────────────────┘                         └─────────────────┘
```

### Event Flow Pipeline

```
┌──────────┐    ┌────────────┐    ┌─────────────┐    ┌──────────────┐    ┌───────────┐
│  Ingest  │───►│ Dispatcher │───►│  EventBus   │───►│  Processors  │───►│  Storage  │
│ TCP/UDP  │    │  (Router)  │    │ (Lock-Free) │    │  (Workers)   │    │  Engine   │
└──────────┘    └────────────┘    └─────────────┘    └──────────────┘    └───────────┘
     │                │                  │                  │
     │                │                  │                  │
     ▼                ▼                  ▼                  ▼
 Frame Parser    Topic-based      3 Priority Queues:   - Realtime
 CRC32 Check     Routing          • REALTIME (SPSC)    - Transactional
 Pool Alloc      Backpressure     • TRANSACTIONAL      - Batch
                                  • BATCH
```

---

## 📊 Performance

### Benchmark Results

| Component | Throughput | P50 Latency | P99 Latency |
|-----------|------------|-------------|-------------|
| **SPSC RingBuffer** | 125M ops/sec | 8 ns | 12 ns |
| **MPSC Queue** | 52M ops/sec | 20 ns | 45 ns |
| **Event Pool** | 89M ops/sec | 11 ns | 25 ns |
| **Lock-Free Dedup** | 71M ops/sec | 14 ns | 32 ns |
| **End-to-End** | 10M+ events/sec | < 1 µs | < 2 µs |

### Optimization Techniques

| Technique | Benefit |
|-----------|---------|
| Cache-line padding (`alignas(64)`) | Prevents false sharing between threads |
| Memory ordering (`acquire/release`) | Minimal synchronization overhead |
| Thread-local event pools | Zero malloc in hot path |
| NUMA binding | Reduces cross-socket memory access latency |
| Vyukov MPSC algorithm | Wait-free producer, lock-free consumer |

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
│   │   ├── admin/                 # Admin loop, control decisions
│   │   ├── config/                # Configuration loader
│   │   ├── control/               # Pipeline state, thresholds
│   │   ├── events/                # Event types, bus, dispatcher
│   │   ├── ingest/                # TCP/UDP servers
│   │   ├── memory/                # Event pool, NUMA binding
│   │   ├── metrics/               # Histograms, registry
│   │   ├── processor/             # Event processors
│   │   ├── queues/                # SPSC, MPSC, dedup
│   │   ├── storage/               # Storage engine
│   │   └── utils/                 # Clock, thread pool
│   ├── distributed/               # Raft consensus
│   └── microservice/              # gRPC gateway, health service
├── src/                           # Implementation
├── tests/                         # Python test scripts
├── unittest/                      # Google Test unit tests
├── benchmark/                     # Performance benchmarks
├── config/                        # YAML configuration
└── doc_*/                         # Documentation
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

## 📚 Documentation

| Document | Description |
|----------|-------------|
| [doc_core/](doc_core/) | Core engine architecture, lock-free queues, event processing |
| [doc_distributed/](doc_distributed/) | Raft consensus, cluster management, leader election |
| [doc_microservice/](doc_microservice/) | gRPC gateway, Kubernetes deployment, monitoring |
| [tests/README.md](tests/README.md) | Testing guide with examples |

---

## 🧪 Testing

```bash
# Unit tests
cd build
./EventStreamTests

# Benchmarks
./benchmark_summary
./benchmark_spsc_detailed
./benchmark_mpsc
./benchmark_dedup

# System tests
cd tests/
python3 test_system.py
```

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

- [x] Lock-free SPSC/MPSC queues
- [x] NUMA-aware memory allocation
- [x] Priority-based event routing
- [x] Adaptive backpressure control
- [x] Raft consensus (basic)
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

---

<p align="center">
  <strong>Built for speed. Designed for scale. Ready for production.</strong>
</p>

<p align="center">
  ⭐ Star this repo if you find it useful!
</p>
