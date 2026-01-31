# � EventStreamCore Documentation

> Ultra-low latency C++17 event streaming engine cho hệ thống real-time.

---

## 📊 Performance Targets

| Metric | Target | Implementation |
|--------|--------|----------------|
| SPSC Queue Push/Pop | < 10ns | Ring Buffer 16384 capacity |
| MPSC Queue Push | < 25ns | Vyukov algorithm, 65536 capacity |
| NUMAEventPool Acquire | < 15ns | O(1) index decrement |
| Lock-Free Dedup | < 20ns | CAS-based hash map 4096 buckets |
| End-to-End Latency | < 5µs P99 | ~2µs measured |

---

## 📁 Documentation Structure

| Document | Description |
|----------|-------------|
| [architecture.md](architecture.md) | System design, data flow, component wiring |
| [queues.md](queues.md) | SPSC, MPSC, Lock-Free Dedup implementations |
| [memory.md](memory.md) | EventPool, NUMAEventPool, IngestEventPool |
| [event.md](event.md) | Event model, priority routing, wire protocol |

---

## 🏗️ Core Components (21 Files)

```
include/eventstream/core/
├── events/                     # Event Model & Bus
│   ├── event.hpp               # Event, EventHeader, EventPriority
│   ├── event_bus.hpp           # EventBusMulti (3 queues)
│   ├── dead_letter_queue.hpp   # DLQ for dropped events
│   ├── dispatcher.hpp          # Priority-based routing
│   ├── event_factory.hpp       # Event creation utilities
│   └── topic_table.hpp         # Topic registry
│
├── queues/                     # Lock-Free Data Structures
│   ├── spsc_ring_buffer.hpp    # Single Producer Single Consumer
│   ├── mpsc_queue.hpp          # Multi Producer Single Consumer
│   └── lock_free_dedup.hpp     # CAS-based deduplication
│
├── memory/                     # Memory Management
│   ├── event_pool.hpp          # Basic event pool
│   ├── numa_event_pool.hpp     # NUMA-aware pool
│   └── numa_binding.hpp        # CPU/Memory affinity
│
├── processor/                  # 3-Tier Processing
│   ├── event_processor.hpp     # Base + Realtime/Trans/Batch
│   ├── process_manager.hpp     # Lifecycle management
│   ├── alert_handler.hpp       # Alert callbacks
│   └── processed_event_stream.hpp
│
├── control/                    # Backpressure Control
│   ├── control_plane.hpp       # Health evaluation
│   ├── pipeline_state.hpp      # State machine
│   └── thresholds.hpp          # Configurable limits
│
├── storage/                    # Persistence
│   └── storage_engine.hpp      # Binary storage + DLQ log
│
├── ingest/                     # Network Input
│   ├── tcp_server.hpp          # Multi-threaded TCP
│   └── udp_server.hpp          # Batch UDP (recvmmsg)
│
├── metrics/                    # Observability
│   ├── registry.hpp            # MetricRegistry
│   └── histogram.hpp           # LatencyHistogram
│
└── admin/                      # Control Interface
    ├── admin_loop.hpp          # Control loop
    └── control_decision.hpp    # Decision types
```

---

## 🔄 Data Flow

```
┌─────────────────┐
│  TCP/UDP Input  │
└────────┬────────┘
         │ parse frame
         ▼
┌─────────────────┐     ┌─────────────────┐
│  IngestEventPool│────►│  Dispatcher     │
│  (NUMAEventPool)│     │  (by priority)  │
└─────────────────┘     └────────┬────────┘
                                 │
         ┌───────────────────────┼───────────────────────┐
         │                       │                       │
         ▼                       ▼                       ▼
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│ REALTIME Queue  │     │ TRANSACTIONAL Q │     │  BATCH Queue    │
│ SPSC 16384 cap  │     │ MPSC 65536 cap  │     │ MPSC 65536 cap  │
│ CRITICAL/HIGH   │     │     MEDIUM      │     │    LOW/BATCH    │
└────────┬────────┘     └────────┬────────┘     └────────┬────────┘
         │                       │                       │
         ▼                       ▼                       ▼
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│RealtimeProcessor│     │TransactionalProc│     │ BatchProcessor  │
│ • AlertHandler  │     │ • LockFreeDedup │     │ • Window Agg    │
│ • < 100µs SLA   │     │ • Retry logic   │     │ • 5s window     │
└────────┬────────┘     └────────┬────────┘     └────────┬────────┘
         │                       │                       │
         └───────────────────────┼───────────────────────┘
                                 ▼
                        ┌─────────────────┐
                        │  StorageEngine  │
                        │ data/events.bin │
                        │ data/dlq_log.txt│
                        └─────────────────┘
```

---

## 🎯 Key Features

### 1. Lock-Free Queues
- **SpscRingBuffer<T, 16384>**: Wait-free, cache-line aligned head/tail
- **MpscQueue<T, 65536>**: Vyukov algorithm with dummy node
- **LockFreeDeduplicator**: 4096 buckets, 1-hour idempotency window

### 2. 3-Tier Processing
| Processor | Priority | Queue | SLA | Features |
|-----------|----------|-------|-----|----------|
| Realtime | CRITICAL/HIGH | SPSC | < 100µs | AlertHandler callback |
| Transactional | MEDIUM | MPSC | < 1ms | Dedup + 3x retry |
| Batch | LOW/BATCH | MPSC | < 10ms | 5s window aggregation |

### 3. 5-Level Backpressure (ControlPlane)
| State | Queue Depth | Drop Rate | Action |
|-------|-------------|-----------|--------|
| HEALTHY | < 50% max | < 1% | Normal |
| ELEVATED | < 75% max | < 2% | Increase workers |
| DEGRADED | < 100% max | < 4% | Pause transactions |
| CRITICAL | = max | ≥ 4% | Drop batch events |
| EMERGENCY | > 150% max | ≥ 10% | Emergency drop |

### 4. NUMA Optimization
- **NUMAEventPool**: Allocate on specific NUMA node
- **NUMABinding**: CPU affinity + memory locality
- **IngestEventPool**: Uses NUMAEventPool internally

---

## 🔧 Configuration (ControlThresholds)

```cpp
struct ControlThresholds {
    uint64_t max_queue_depth = 5000;         // Trigger action threshold
    double max_drop_rate = 2.0;              // Max acceptable drop %
    uint64_t max_latency_ms = 100;           // Future use
    uint64_t min_events_for_evaluation = 1000; // Warmup period
    double recovery_factor = 0.8;            // Hysteresis (80%)
};
```

---

## 🚀 Quick Start

```cpp
#include <eventstream/core/events/event_bus.hpp>
#include <eventstream/core/processor/process_manager.hpp>
#include <eventstream/core/storage/storage_engine.hpp>

int main() {
    // 1. Create infrastructure
    EventStream::EventBusMulti bus;
    StorageEngine storage("data/events.bin");
    
    // 2. Wire dependencies
    ProcessManager::Dependencies deps;
    deps.storage = &storage;
    deps.dlq = &bus.getDLQ();
    deps.batch_window = std::chrono::seconds(5);
    
    // 3. Start processing
    ProcessManager pm(bus, deps);
    pm.start();
    
    // 4. Push events by priority
    auto event = std::make_shared<EventStream::Event>();
    event->header.priority = EventStream::EventPriority::HIGH;
    bus.push(EventStream::EventBusMulti::QueueId::REALTIME, event);
    
    // 5. Cleanup
    pm.stop();
    return 0;
}
```

---

## 🐛 Bugs Fixed (Session Review)

| # | Component | Issue | Fix |
|---|-----------|-------|-----|
| 1 | LockFreeDedup | Race in cleanup head update | Use CAS for atomic update |
| 2 | ControlPlane | Static previous_state not thread-safe | Move to member variable |
| 3 | ControlPlane | min_events_for_evaluation unused | Add warmup check |
| 4 | EventBus | REALTIME dropBatch not pushing DLQ | Push batch to DLQ |
| 5 | TCPServer | Missing backpressure stats | Add totalBackpressureDrops_ |
| 6 | MetricRegistry | metrics_map_ public | Move to private |
| 7 | TransactionalProc | Dedup insert before success | Insert only on success |
| 8 | BatchProcessor | Duplicate bucket.events.clear() | Remove duplicate |
| 9 | main.cpp | StorageEngine not wired | Wire to ProcessManager::Dependencies |
| 10 | StorageEngine | DLQ path hardcoded | Derive from storage path |
| 11 | NUMAEventPool | release() broken | Proper search + NUMA cleanup |
| 12 | IngestEventPool | Using EventPool | Switch to NUMAEventPool |
