# EventStreamCore

High-performance, lock-free event processing engine built for **82K+ events/sec** with **p99 latency < 5ms**.

## Features

✅ **Lock-Free Hot Paths** - SPSC ring buffers (3.7M eps), lock-free dedup, async metrics (95% contention reduction)
✅ **Pre-Allocated Memory** - 65K events pool, zero malloc in hot path, 100-130 MB RSS
✅ **Zero-Copy Ingest** - Direct offset parsing, 2.5GB/sec bandwidth savings
✅ **3 Processor Types** - Realtime (low-latency), Transactional (idempotent), Batch (windowed)
✅ **Pluggable Storage** - Memory, File, SQL, Kafka backends

## Quick Start

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release && cd build && make -j4

# Test (38 tests)
./unittest/EventStreamTests

# Run
./EventStreamCore
```

## Performance

| Metric | Value |
|--------|-------|
| Throughput | 82K events/sec |
| Latency p50 | 50 μs |
| Latency p99 | 5 ms |
| Memory | 100-130 MB |
| CPU | 85% (15% spare) |

## Architecture

```
TCP Input → EventPool → EventBus → [Realtime|Transactional|Batch] → Storage
                                   (Lock-Free SPSC Queues)
```

## Day 39 Optimizations

| Optimization | Impact |
|--------------|--------|
| Async metric timestamps (1ms buffering) | 10-15% CPU ↓ |
| String_view overloads | 2-3% CPU ↓ |
| Batch map consolidation | 1-2% CPU ↓ |
| TCP zero-copy parsing | 5-8% CPU ↓ |
| Vector pre-allocation | 1-2% CPU ↓ |
| False sharing fix (alignas) | 2-5% ↑ |
| **Total** | **6-20% CPU ↓** |

## Documentation

See `engine_documents/` for detailed docs:
- **CORE_ENGINE_TECHNICAL_DOCUMENTATION.md** - Complete design
- **SEQUENCE_DIAGRAMS.md** - Flow diagrams (PlantUML)
- **CORE_ENGINE_ASSESSMENT.md** - Performance analysis
- **CORE_ENGINE_QUICK_REFERENCE.md** - Quick lookup

## Key Components

### EventPool
Pre-allocated 65K events, O(1) acquire/release. No malloc in hot path.

### SPSC Ring Buffers
Lock-free point-to-point queues. 3.7M+ events/sec per queue.

### Lock-Free Deduplicator
Hash table with CAS-based insertion. 1M+ checks/sec, no locks on read.

### Metrics Registry
Thread-local 1ms buffering. 246K → 1K lock ops/sec (95% reduction).

## Configuration

Edit `config/config.yaml`:
- Event pool capacity: 65536
- Queue capacities: 16384
- Processor threads: Realtime/Transactional/Batch (1 each)
- Storage backend: memory/file/sql/kafka

## Testing

```bash
cd build
./unittest/EventStreamTests        # 38 tests
./benchmark/benchmark_spsc         # 3.7M eps
./benchmark/benchmark_dedup        # 1M checks/sec
```

## Deployment

```bash
# Single node
./EventStreamCore

# With monitoring
tail -f logs/eventstream.log
```

## Status

✅ **Production-Ready** (Single Node)
- All 38 tests passing
- Zero warnings, clean build
- Ready for distributed layer

## Next: Distributed Layer

- Event replication (async, batched)
- Distributed dedup (consistent hashing)
- Automatic failover (RAFT)
- Multi-region support

---

**More details in `engine_documents/` folder**

