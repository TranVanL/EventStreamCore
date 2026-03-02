# 01 — Architecture Overview

> **Goal:** Understand the entire system at a glance — every layer, every component, every data path.

---

## 1. High-Level Architecture

```
                    ┌──────────────────────────────────────────────┐
                    │              INGEST LAYER                    │
                    │  ┌──────────┐  ┌──────────┐                 │
                    │  │TCP Server│  │UDP Server│  (+ future File)│
                    │  └────┬─────┘  └────┬─────┘                 │
                    │       │             │                        │
                    │       ▼             ▼                        │
                    │  ┌───────────────────────┐                  │
                    │  │  IngestEventPool       │  ← object pool  │
                    │  └───────────┬───────────┘                  │
                    └──────────────┼──────────────────────────────┘
                                   │ EventPtr
                    ┌──────────────▼──────────────────────────────┐
                    │              DISPATCH LAYER                  │
                    │  ┌────────────────────────────┐             │
                    │  │  MPSC Queue (65 536 slots)  │  lock-free │
                    │  └──────────┬─────────────────┘             │
                    │             │ single consumer               │
                    │  ┌──────────▼─────────────────┐             │
                    │  │  Dispatcher                 │             │
                    │  │  • TopicTable lookup         │             │
                    │  │  • Priority promotion        │             │
                    │  │  • Pressure adaptation       │             │
                    │  │  • route() → QueueId         │             │
                    │  └──────────┬─────────────────┘             │
                    └──────────────┼──────────────────────────────┘
                                   │ push(QueueId, EventPtr)
                    ┌──────────────▼──────────────────────────────┐
                    │              EVENT BUS                       │
                    │  ┌─────────────────────────────────────┐    │
                    │  │  EventBusMulti                       │    │
                    │  │  ┌─────────┐ ┌──────────┐ ┌───────┐│    │
                    │  │  │REALTIME │ │TRANSACT. │ │ BATCH ││    │
                    │  │  │SPSC 16k │ │deque 131k│ │deq 32k││    │
                    │  │  └────┬────┘ └────┬─────┘ └───┬───┘│    │
                    │  └───────┼───────────┼───────────┼────┘    │
                    │          │           │           │          │
                    │  ┌───────▼───────────▼───────────▼────┐    │
                    │  │        DeadLetterQueue (DLQ)        │    │
                    │  └────────────────────────────────────┘    │
                    └─────────────────────────────────────────────┘
                                   │ pop(QueueId)
                    ┌──────────────▼──────────────────────────────┐
                    │              PROCESSOR LAYER                 │
                    │  ┌─────────────────────────────────────┐    │
                    │  │  ProcessManager                      │    │
                    │  │  ┌──────────┐┌────────────┐┌──────┐│    │
                    │  │  │Realtime  ││Transaction.││Batch ││    │
                    │  │  │(alerts   ││(dedup,     ││(wind.││    │
                    │  │  │ SLA)     ││ retry)     ││flush)││    │
                    │  │  └──────────┘└────────────┘└──────┘│    │
                    │  └─────────────────────────────────────┘    │
                    │          │           │           │           │
                    │  ┌───────▼───────────▼───────────▼────┐     │
                    │  │      StorageEngine (binary file)    │     │
                    │  └────────────────────────────────────┘     │
                    └─────────────────────────────────────────────┘
                    ┌─────────────────────────────────────────────┐
                    │              CONTROL PLANE                   │
                    │  ┌──────┐ ┌────────────┐ ┌───────────────┐  │
                    │  │Admin │→│ControlPlane│→│PipelineState   │  │
                    │  │(10s) │ │ evaluate() │ │Manager (atomic)│  │
                    │  └──────┘ └────────────┘ └───────────────┘  │
                    └─────────────────────────────────────────────┘
                    ┌─────────────────────────────────────────────┐
                    │              C API BRIDGE                    │
                    │  esccore.h  →  libesccore.so                │
                    │  ↕ Python SDK (ctypes)                      │
                    │  ↕ Go SDK (cgo / dlopen)                    │
                    └─────────────────────────────────────────────┘
```

---

## 2. Component Inventory

| Component | Header | Source | Purpose |
|-----------|--------|--------|---------|
| **Event / EventHeader** | `events/event.hpp` | — | Core data type: id, priority, topic, body, metadata, CRC |
| **EventFactory** | `events/event_factory.hpp` | `events/event_factory.cpp` | Atomic ID generation, CRC-32 computation, event construction |
| **TopicTable** | `events/topic_table.hpp` | `events/topic_table.cpp` | Config-file-driven topic→priority lookup (shared_mutex) |
| **Dispatcher** | `events/dispatcher.hpp` | `events/dispatcher.cpp` | MPSC ingest → single-thread routing → EventBus push |
| **EventBusMulti** | `events/event_bus.hpp` | `events/event_bus.cpp` | 3-queue fan-out (REALTIME/TRANSACTIONAL/BATCH) + overflow + DLQ |
| **DeadLetterQueue** | `events/dead_letter_queue.hpp` | `events/dead_letter_queue.cpp` | Dropped-event ring buffer (last 1000) + atomic counter |
| **SpscRingBuffer** | `queues/spsc_ring_buffer.hpp` | `queues/spsc_ring_buffer.cpp` | Lock-free SPSC, power-of-2 masking, cache-line-aligned |
| **MpscQueue** | `queues/mpsc_queue.hpp` | — (header-only) | Vyukov-style intrusive MPSC with dummy node |
| **LockFreeDeduplicator** | `queues/lock_free_dedup.hpp` | `queues/lock_free_dedup.cpp` | CAS-based hash map for idempotency, 4096 buckets, 1h window |
| **EventPool** | `memory/event_pool.hpp` | — | Template object pool with SFINAE pool_index_ detection |
| **IngestEventPool** | `ingest/ingest_pool.hpp` | — | `shared_ptr` custom-deleter pool for ingest layer |
| **NUMABinding** | `memory/numa_binding.hpp` | — | libnuma wrappers: bind thread, allocate on node, topology print |
| **TcpIngestServer** | `ingest/tcp_server.hpp` | `ingest/tcp_server.cpp` | Blocking `accept()` + per-client thread, length-prefixed frames |
| **UdpIngestServer** | `ingest/udp_server.hpp` | `ingest/udp_server.cpp` | Blocking `recvfrom()`, single-thread datagram parsing |
| **FrameParser** | `ingest/frame_parser.hpp` | `ingest/frame_parser.cpp` | Wire-format: `[4B len][1B priority][2B topic_len][topic][payload]` |
| **EventProcessor (base)** | `processor/event_processor.hpp` | — | ABC with `start/stop/process/name` + NUMA affinity |
| **RealtimeProcessor** | (same header) | `processor/realtime_processor.cpp` | SLA enforcement (ms), alert emission, storage |
| **TransactionalProcessor** | (same header) | `processor/transactional_processor.cpp` | Lock-free dedup, exponential retry, latency histogram |
| **BatchProcessor** | (same header) | `processor/batch_processor.cpp` | Topic-bucketed windowed flush (default 5s) |
| **ProcessManager** | `processor/process_manager.hpp` | `processor/process_manager.cpp` | Thread-per-processor orchestrator, NUMA pinning |
| **AlertHandler (hierarchy)** | `processor/alert_handler.hpp` | — | Logging / Callback / Composite / Null alert handlers |
| **ProcessedEventStream** | `processor/processed_event_stream.hpp` | — | Observer-based notification after process or drop |
| **StorageEngine** | `storage/storage_engine.hpp` | `storage/storage_engine.cpp` | Append-only binary file + DLQ text log |
| **AppConfiguration** | `config/app_config.hpp` | — | POD config structs (TCP, UDP, File, Router, NUMA…) |
| **ConfigLoader** | `config/loader.hpp` | `config/config_loader.cpp` | yaml-cpp loader with validation |
| **Metrics / MetricSnapshot** | `metrics/metrics.hpp` | — | Atomic counters + snapshot struct |
| **LatencyHistogram** | `metrics/histogram.hpp` | — | Log₂-bucket histogram, percentile calculation |
| **MetricRegistry** | `metrics/registry.hpp` | `metrics/registry.cpp` | Global singleton, snapshots, health evaluation |
| **ControlPlane** | `control/control_plane.hpp` | `control/control_plane.cpp` | 5-level state machine: HEALTHY → ELEVATED → DEGRADED → CRITICAL → EMERGENCY |
| **PipelineStateManager** | `control/pipeline_state.hpp` | `control/pipeline_state.cpp` | Atomic state enum (RUNNING/PAUSED/DRAINING/DROPPING/EMERGENCY) |
| **ControlThresholds** | `control/thresholds.hpp` | — | Tuning knobs (max queue, drop rate, latency, recovery factor) |
| **Admin** | `admin/admin_loop.hpp` | `admin/admin_loop.cpp` | 10-second monitoring loop, metric aggregation, control actions |
| **Clock** | `utils/clock.hpp` | — | `steady_clock` wrappers: `now_ns`, `now_us`, `now_ms` |
| **ThreadPool** | `utils/thread_pool.hpp` | `utils/thread_pool.cpp` | Classic condition-variable work queue |
| **C API Bridge** | `bridge/esccore.h` | `bridge/esccore.cpp` | C ABI for FFI, wraps entire engine lifecycle |

---

## 3. Data Flow — End to End

```
Network byte stream
        │
        ▼
  TcpIngestServer::handleClient()  /  UdpIngestServer::receiveLoop()
        │  reads length-prefix frame
        │  parseFrameBody() → ParsedFrame
        │  EventFactory::createEvent() → Event  (atomic id, CRC-32)
        │  IngestEventPool::acquireEvent() → shared_ptr<Event> with custom deleter
        ▼
  Dispatcher::tryPush(EventPtr)
        │  MpscQueue::push()  — lock-free, multiple producers
        ▼
  Dispatcher::dispatchLoop()  (single thread)
        │  MpscQueue::pop()
        │  TopicTable::findTopic() → upgrade priority?
        │  adaptToPressure() → downgrade HIGH→MEDIUM under load?
        │  route() → QueueId {REALTIME, TRANSACTIONAL, BATCH}
        │  EventBusMulti::push(queueId, evt)  — with retry + exp backoff
        │       failed 3× → DLQ
        ▼
  ProcessManager::runLoop(queueId, processor)  (one thread per processor)
        │  EventBusMulti::pop(queueId, timeout)
        │  processor->process(event)
        ▼
  RealtimeProcessor    │  TransactionalProcessor    │  BatchProcessor
  • SLA check (ms)     │  • dedup (CAS hash map)    │  • bucket by topic
  • alert emission     │  • retry 3× (exp backoff)  │  • flush on window
  • storage            │  • latency histogram        │  • aggregate stats
  • DLQ on failure     │  • storage, DLQ             │  • storage, DLQ
        │                       │                           │
        ▼                       ▼                           ▼
  StorageEngine::storeEvent()  — append binary record
  ProcessedEventStream::notifyProcessed()  — observer pattern
```

---

## 4. Startup Sequence (main.cpp)

```
main()
  ├── setupLogging()                         // spdlog pattern
  ├── setupSignalHandlers()                  // SIGINT/SIGTERM → atomic flag
  ├── loadConfiguration(argc, argv)          // yaml-cpp → AppConfiguration
  ├── IngestEventPool::initialize()          // pre-allocate 10 000 Event objects
  ├── initializeComponents(config)
  │     ├── new EventBusMulti
  │     ├── new Dispatcher(bus)
  │     ├── TopicTable::loadFromFile()
  │     ├── new StorageEngine(path)
  │     ├── new ProcessManager(bus, deps)    // creates 3 processors
  │     ├── new TcpIngestServer (if enabled)
  │     ├── new UdpIngestServer (if enabled)
  │     └── new Admin(pm)                    // creates ControlPlane + PipelineState
  ├── startComponents()
  │     ├── dispatcher.start()               // spawns dispatch thread
  │     ├── processManager.start()           // spawns 3 processor threads
  │     ├── tcpServer.start()                // spawns accept thread
  │     ├── udpServer.start()                // spawns receive thread
  │     └── admin.start()                    // spawns 10s monitoring thread
  ├── while (g_running) sleep(500ms)
  └── stopComponents()
        ├── admin.stop()
        ├── udpServer.stop()
        ├── tcpServer.stop()
        ├── processManager.stop()
        └── dispatcher.stop()
```

**Key insight:** Shutdown is in *reverse startup order* to ensure upstream producers stop before downstream consumers, preventing event loss.

---

## 5. Thread Model

| Thread | Pinned CPU | Role |
|--------|-----------|------|
| main | — | Config, lifecycle, signal wait |
| TCP accept | — | `accept()` loop, spawns client threads |
| TCP client (N) | — | Frame parsing, push to Dispatcher |
| UDP receive | — | `recvfrom()` loop, push to Dispatcher |
| Dispatcher | — | MPSC→route→EventBus (single consumer) |
| RealtimeProcessor | Core 2 (hard-coded) | Pop REALTIME queue, SLA check |
| TransactionalProcessor | — | Pop TRANSACTIONAL queue, dedup + retry |
| BatchProcessor | — | Pop BATCH queue, windowed flush |
| Admin | — | 10s sleep, metrics aggregation, control plane |

Total steady-state threads: **8 + N** (where N = concurrent TCP clients).

---

## 6. Ownership & Lifetime

```
main() owns:
  ├── EventBusMulti           (unique_ptr)
  ├── Dispatcher              (unique_ptr)  → borrows EventBus&
  ├── StorageEngine           (unique_ptr)
  ├── ProcessManager          (unique_ptr)  → borrows EventBus&
  │     ├── RealtimeProcessor       (unique_ptr)  → borrows Storage*, DLQ*
  │     ├── TransactionalProcessor  (unique_ptr)  → borrows Storage*, DLQ*
  │     └── BatchProcessor          (unique_ptr)  → borrows Storage*, EventBus*, DLQ*
  ├── TcpIngestServer         (unique_ptr)  → borrows Dispatcher&
  ├── UdpIngestServer         (unique_ptr)  → borrows Dispatcher&
  └── Admin                   (unique_ptr)  → borrows ProcessManager&
        ├── PipelineStateManager  (member)
        └── ControlPlane          (unique_ptr)
```

**Rule:** All raw pointers are *non-owning* observers; `unique_ptr` in `main()` guarantees lifetime.

---

## 7. Configuration (YAML)

```yaml
app_name: "EventStreamCore"
version: "1.0.0"
ingestion:
  tcp:  { host: "0.0.0.0", port: 9001, enable: true, maxConnections: 100 }
  udp:  { host: "0.0.0.0", port: 9002, enable: true, bufferSize: 65536 }
  file: { path: "/var/log/events", enable: false, poll_interval_ms: 1000 }
router:  { shards: 4, strategy: "round_robin", buffer_size: 4096 }
rule_engine: { enable_cache: true, rules_file: "rules.yaml", threads: 2, cache_size: 1024 }
storage: { backend: "sqlite", sqlite_path: "data/events.db" }
numa: { enable: false, dispatcher_node: 0, ingest_node: 0, realtime_proc_node: 1, ... }
```

---

## 8. Interview Talking Points

1. **"Walk me through the architecture."** → Use the diagram in §1; emphasise the lock-free MPSC hand-off between ingest and dispatch.
2. **"How do you handle back-pressure?"** → 5-level cascade: EventBus pressure → Dispatcher downgrade → ControlPlane actions → PipelineState → processor pause/drop.
3. **"What happens when the system is overloaded?"** → Admin loop detects metrics, ControlPlane decides action (PAUSE → DROP_BATCH → PUSH_DLQ), PipelineStateManager stores atomic state read by Dispatcher.
4. **"How do you avoid contention?"** → SPSC ring buffer for realtime (no lock), MPSC for ingest→dispatch, per-topic mutexes in batch.
5. **"Explain the shutdown order."** → Reverse of startup; upstream first to drain, then processors, then storage flush.
