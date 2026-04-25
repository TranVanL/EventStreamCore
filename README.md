# EventStreamCore

Low-latency event streaming engine written in C++17. Ships with a C API (`libesccore.so`) so you can use it from Python, Go, or link directly in C++.

## What it does

- Ingests events over TCP/UDP
- Routes them through lock-free queues (SPSC + MPSC)
- Processes with three priority tiers: realtime, transactional, batch
- Persists to a binary storage engine with a dead-letter queue for failures
- Exposes a C API that Python and Go SDKs wrap

## Architecture

```
Ingest (TCP/UDP)
    |
    v
Dispatcher --> EventBus (3 queues) --> Processors --> StorageEngine
                                            |
                                           DLQ
```

The core owns all the hot paths. SDKs just call through `libesccore.so`.

### Key components

| Component | What it does |
|-----------|-------------|
| `SpscRingBuffer<T, 16384>` | Lock-free SPSC queue |
| `MpscQueue<T, 65536>` | Vyukov-style MPSC queue |
| `LockFreeDeduplicator` | CAS-based dedup with 1h TTL |
| `NUMAEventPool` | Pre-allocated pool with NUMA binding |
| `ControlPlane` | 5-level backpressure (healthy to emergency) |
| `StorageEngine` | Binary persistence + DLQ |

## Quick start

### Dependencies

```bash
# Ubuntu/Debian
sudo apt-get install -y build-essential cmake libspdlog-dev libyaml-cpp-dev libnuma-dev
```

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This produces:
- `build/EventStreamCore` — standalone server
- `build/src/bridge/libesccore.so` — shared lib for SDKs

### Run

```bash
./EventStreamCore ../config/config.yaml
```

## SDKs

### Python

```python
from esccore import Engine, Priority

with Engine("build/libesccore.so") as engine:
    engine.init("config/config.yaml")
    engine.push("sensor/temp", b"\x42", Priority.HIGH)
    print(engine.metrics())
```

There's also a FastAPI adapter:

```bash
ESCCORE_LIB=build/libesccore.so esccore-adapter
```

### Go

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

gRPC adapter:

```bash
go run ./cmd/grpc-adapter -lib build/libesccore.so -port 50051
```

### C++ (direct linkage)

```cpp
#include <eventstream/bridge/esccore.h>

esccore_init("config/config.yaml");
esc_event_t evt = { .id=1, .priority=ESC_PRIORITY_HIGH,
                     .topic="sensor/temp", .body=data, .body_len=4 };
esccore_push(&evt);
esccore_shutdown();
```

## Benchmarks

Run after building:

```bash
cd build
./benchmark_spsc_detailed
./benchmark_mpsc
./benchmark_dedup
./benchmark_event_pool
./benchmark_eventbus_multi
./benchmark_summary
```

Rough numbers on a typical dev box:

| Component | Throughput | P99 |
|-----------|-----------|-----|
| SPSC RingBuffer | ~125M ops/s | ~12 ns |
| MPSC Queue | ~52M ops/s | ~45 ns |
| LockFreeDedup | ~71M ops/s | ~32 ns |
| EventPool | ~89M ops/s | ~25 ns |

## Tests

```bash
cd build && ./EventStreamTests
```

Python integration:

```bash
cd tests && python3 stress_test.py 127.0.0.1 9000 10 10000
```

## Project layout

```
include/eventstream/
    core/           # Engine headers (queues, memory, events, processors, etc.)
    bridge/         # C API header (esccore.h)
src/
    core/           # Implementation
    bridge/         # C API implementation
    main.cpp        # Server entry point
sdk/
    python/         # Python SDK + FastAPI adapter
    go/             # Go SDK + gRPC adapter
unittest/           # Google Test
benchmark/          # Perf benchmarks
config/             # YAML config + topic routing
```

## TODO

- [ ] Protobuf definitions for gRPC
- [ ] WebSocket streaming in Python adapter
- [ ] Prometheus push-gateway
- [ ] Kubernetes operator
- [ ] Rust SDK

## License

MIT
