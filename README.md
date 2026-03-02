

<h1 align="center">⚡ EventStreamCore</h1>

<p align="center">
  <strong>Ultra-Low Latency Event Streaming Engine</strong><br>
  <em>High-performance C++17 core with polyglot SDKs (Python · Go · C++)</em>
</p>

<p align="center">
  <a href="#-features">Features</a> •
  <a href="#-architecture">Architecture</a> •
  <a href="#-core-components">Components</a> •
  <a href="#-polyglot-sdks">SDKs</a> •
  <a href="#-performance">Performance</a> •
  <a href="#-quick-start">Quick Start</a>
</p>

---

## 🎯 Overview

**EventStreamCore** is a production-grade event streaming engine built for
systems that demand **microsecond-level latency** and **millions of events per
second**.  The C++17 core does the heavy lifting; lightweight SDKs in
**Python**, **Go**, and **C++** let you integrate it into any stack.

| Domain | Use Cases |
|--------|-----------|
| 🏦 **Financial Systems** | Order matching, market data feeds, risk calculation |
| 🌐 **IoT Platforms** | Sensor data aggregation, real-time telemetry |
| 🎮 **Gaming Backends** | Player events, matchmaking, leaderboards |
| 📊 **Real-time Analytics** | Stream processing, CEP (Complex Event Processing) |

---

## ✨ Features

### Core Engine (C++17)

- **🚀 Ultra-Low Latency** — P99 < 2 µs with lock-free queues
- **📈 High Throughput** — 10 M+ events / sec on commodity hardware
- **🔒 Lock-Free Design** — SPSC (16 384) / MPSC (65 536) queues
- **💾 Zero-Allocation Hot Path** — NUMA-aware pre-allocated pools
- **🖥️ NUMA-Aware** — Thread affinity + memory binding for multi-socket
- **🔄 Deduplication** — 1-hour idempotency window (4 096-bucket hash map)
- **📉 5-Level Backpressure** — HEALTHY → ELEVATED → DEGRADED → CRITICAL → EMERGENCY
- **🗄️ Dead Letter Queue** — Failed events persisted for replay

### Polyglot SDK

- **🐍 Python** — ctypes wrapper + FastAPI REST adapter + Prometheus exporter
- **🐹 Go** — cgo wrapper + gRPC adapter + Kubernetes health probes
- **⚙️ C++** — Direct header-only linkage (zero overhead)

---

## 🏗️ Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                    EventStreamCore  (C++17)                       │
│  ┌──────────┐  ┌────────────┐  ┌──────────┐  ┌──────────────┐  │
│  │  Ingest   │→│ Dispatcher  │→│ EventBus  │→│  Processors   │  │
│  │ TCP / UDP │  │  (Router)  │  │(Lock-Free)│  │ RT / TX / BA │  │
│  └──────────┘  └────────────┘  └──────────┘  └──────┬───────┘  │
│       ↑              ↑              ↑                │          │
│  NUMAEventPool  TopicTable   SPSC / MPSC     StorageEngine     │
│                 BackPressure  3 Queues          + DLQ           │
│                 ControlPlane                                     │
├──────────────────────────────────────────────────────────────────┤
│                   C API  (libesccore.so)                         │
│  esccore_init · esccore_push · esccore_metrics · esccore_health  │
└───────┬──────────────────┬───────────────────┬──────────────────┘
        │                  │                   │
   ┌────▼─────┐      ┌────▼─────┐       ┌─────▼────┐
   │  Python   │      │    Go    │       │   C++    │
   │   SDK     │      │   SDK    │       │  (link)  │
   ├──────────┤      ├──────────┤       └──────────┘
   │ FastAPI   │      │  gRPC    │
   │ Prometheus│      │  K8s     │
   │ REST/WS   │      │  Metrics │
   └──────────┘      └──────────┘
```

> **Design Principle**: The C++ core is the ⭐ *star* — it owns all the
> performance-critical paths. SDKs and adapters are thin supporters that
> translate the core's C API into each language's idioms and ecosystem tools.

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
| | `MpscQueue<T,65536>` | Vyukov MPSC (wait-free producer) |
| | `LockFreeDeduplicator` | 4 096-bucket hash map, 1 h idempotency |
| **Memory** | `NUMAEventPool<T,N>` | NUMA-aware pre-allocated object pool |
| | `NUMABinding` | CPU affinity + NUMA node binding |
| | `IngestEventPool` | Thread-safe shared pool for TCP/UDP |
| **Processing** | `RealtimeProcessor` | Alert handler for CRITICAL / HIGH priority |
| | `TransactionalProcessor` | Dedup + 3-retry with exponential backoff |
| | `BatchProcessor` | 5-second aggregation window |
| **Control** | `ControlPlane` | 5-level backpressure (HEALTHY → EMERGENCY) |
| | `AdminLoop` | Periodic health check + cleanup |
| **Storage** | `StorageEngine` | Binary event persistence + DLQ |
| **Ingest** | `TcpIngestServer` | Multi-client TCP with backpressure |
| | `UdpIngestServer` | High-throughput UDP receiver |
| **Bridge** | `esccore.h` | C API for polyglot SDK consumption |

---

## 🌍 Polyglot SDKs

### Python SDK (`sdk/python/`)

```python
from esccore import Engine, Priority

with Engine("build/libesccore.so") as engine:
    engine.init("config/config.yaml")
    engine.push("sensor/temp", b"\x42", Priority.HIGH)
    print(engine.metrics())
```

**FastAPI adapter** — REST + Prometheus in one command:

```bash
ESCCORE_LIB=build/libesccore.so esccore-adapter
# POST /events, GET /metrics, GET /health
```

### Go SDK (`sdk/go/`)

```go
engine, _ := esc.New("build/libesccore.so")
engine.Init("config/config.yaml")
defer engine.Shutdown()

engine.Push(esc.Event{
    Topic:    "sensor/temperature",
    Body:     []byte{0x42},
    Priority: esc.PriorityHIGH,
})
```

**gRPC adapter** — binary protocol + K8s probes:

```bash
go run ./cmd/grpc-adapter -lib build/libesccore.so -port 50051
```

### C++ (direct link)

```cpp
#include <eventstream/bridge/esccore.h>

esccore_init("config/config.yaml");
esc_event_t evt = { .id=1, .priority=ESC_PRIORITY_HIGH,
                     .topic="sensor/temp", .body=data, .body_len=4 };
esccore_push(&evt);
esccore_shutdown();
```

---

## 📊 Performance

| Component | Throughput | P50 | P99 | Capacity |
|-----------|-----------|-----|-----|----------|
| **SPSC RingBuffer** | 125 M ops/s | 8 ns | 12 ns | 16 384 |
| **MPSC Queue** | 52 M ops/s | 20 ns | 45 ns | 65 536 |
| **NUMAEventPool** | 89 M ops/s | 11 ns | 25 ns | Configurable |
| **Lock-Free Dedup** | 71 M ops/s | 14 ns | 32 ns | 4 096 buckets |
| **End-to-End** | 10 M+ events/s | < 1 µs | < 2 µs | — |

---

## 🚀 Quick Start

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt-get install -y build-essential cmake libspdlog-dev libyaml-cpp-dev libnuma-dev

# Python SDK
pip install -e sdk/python

# Go SDK — just `go build` (cgo links libesccore.so automatically)
```

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Produces:
#   build/EventStreamCore              ← standalone C++ server
#   build/src/bridge/libesccore.so     ← shared library for SDKs
```

### Run

```bash
# Standalone C++ server
./EventStreamCore ../config/config.yaml

# Python REST adapter
ESCCORE_LIB=build/src/bridge/libesccore.so esccore-adapter

# Go gRPC adapter
cd sdk/go && go run ./cmd/grpc-adapter -lib ../../build/src/bridge/libesccore.so
```

---

## 📁 Project Structure

```
EventStreamCore/
├── include/eventstream/
│   ├── core/                    # ⭐ Core engine headers
│   │   ├── admin/               # AdminLoop, ControlPlane
│   │   ├── config/              # ConfigLoader, AppConfig
│   │   ├── control/             # PipelineState, Thresholds
│   │   ├── events/              # Event, EventBus, Dispatcher, DLQ
│   │   ├── ingest/              # TcpIngestServer, UdpIngestServer
│   │   ├── memory/              # NUMAEventPool, NUMABinding
│   │   ├── metrics/             # Histogram, MetricRegistry
│   │   ├── processor/           # Realtime / Transactional / Batch
│   │   ├── queues/              # SPSC, MPSC, LockFreeDedup
│   │   ├── storage/             # StorageEngine
│   │   └── utils/               # Clock, ThreadPool
│   └── bridge/
│       └── esccore.h            # C API — universal SDK interface
├── src/
│   ├── core/                    # C++ implementations
│   ├── bridge/                  # esccore.cpp (C API → core)
│   └── main.cpp                 # Standalone server entry point
├── sdk/
│   ├── python/                  # 🐍 Python SDK + FastAPI adapter
│   └── go/                      # 🐹 Go SDK + gRPC adapter
├── tests/                       # Python integration tests
├── unittest/                    # Google Test unit tests
├── benchmark/                   # Performance benchmarks
├── config/                      # YAML configuration
└── doc_core/                    # Core documentation
```

---

## 🧪 Testing

```bash
# C++ unit tests
cd build && ./EventStreamTests

# Benchmarks
./benchmark_spsc_detailed
./benchmark_mpsc
./benchmark_dedup
./benchmark_event_pool
./benchmark_eventbus_multi
./benchmark_summary

# Python integration
cd tests && python3 stress_test.py 127.0.0.1 9000 10 10000
```

---

## 🛠️ Tech Stack

| Layer | Technology |
|-------|------------|
| **Core Engine** | C++17, lock-free atomics, NUMA |
| **Build** | CMake 3.10+ |
| **Logging** | spdlog |
| **Config** | yaml-cpp |
| **Testing** | Google Test |
| **Python SDK** | ctypes, FastAPI, prometheus-client |
| **Go SDK** | cgo, gRPC, net/http |
| **Platform** | Linux (NUMA support) |

---

## 🗺️ Roadmap

- [x] Lock-free SPSC / MPSC queues
- [x] NUMA-aware memory allocation
- [x] Priority-based 3-queue routing
- [x] 5-Level adaptive backpressure
- [x] Lock-free deduplication
- [x] Binary storage + Dead Letter Queue
- [x] C API bridge (`libesccore.so`)
- [x] Python SDK + FastAPI adapter
- [x] Go SDK + gRPC adapter scaffold
- [ ] Proto definitions for gRPC service
- [ ] WebSocket streaming in Python adapter
- [ ] Prometheus push-gateway support
- [ ] Kubernetes Operator (Go)
- [ ] Rust SDK via FFI

---

## 📄 License

MIT License — see [LICENSE](LICENSE) for details.

<p align="center">⭐ Star this repo if you find it useful!</p>
