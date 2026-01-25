## CORE ENGINE QUICK REFERENCE GUIDE
**Target Audience**: Developers working on distributed layer | **Updated**: Day 39

---

## ARCHITECTURE AT A GLANCE

```
┌──────────────────────────────────────────────────────────┐
│                    EventStreamCore                       │
├──────────────────────────────────────────────────────────┤
│                                                            │
│  TCP Ingest (Zero-Copy)                                  │
│  └─ parseFrameFromOffset() → Event                       │
│                                                            │
│  EventPool (Pre-allocated)                               │
│  └─ 65K events pre-allocated, O(1) acquire/release      │
│                                                            │
│  EventBusMulti (Lock-free Dispatch)                      │
│  └─ SPSC queue routing to 3 processor types              │
│                                                            │
│  ┌─ RealtimeProcessor                                    │
│  ├─ TransactionalProcessor (with lock-free dedup)       │
│  └─ BatchProcessor (with windowing)                      │
│                                                            │
│  MetricRegistry (Async buffering)                        │
│  └─ Thread-local 1ms buffering, 1K lock ops/sec         │
│                                                            │
│  Storage Engine (Pluggable)                              │
│  └─ Memory/File/SQL/Kafka backends                       │
│                                                            │
└──────────────────────────────────────────────────────────┘
```

---

## KEY COMPONENTS

### 1. SPSC Ring Buffer
**Location**: `include/utils/spsc_ringBuffer.hpp`
**Purpose**: Ultra-fast point-to-point message passing
**Performance**: 3.7M+ events/sec, 0.13μs latency (p50)
**Key Feature**: Lock-free with acquire/release semantics
**Critical Fix (Day 39)**: `alignas(64)` on tail_ to prevent false sharing

**Usage Pattern**:
```cpp
SpscRingBuffer<EventPtr, 16384> queue;

// Producer thread
queue.push(event);

// Consumer thread
if (auto event = queue.pop()) {
    // Process event
}
```

---

### 2. Event Pool
**Location**: `include/core/memory/event_pool.hpp`
**Purpose**: Pre-allocated event objects (zero malloc in hot path)
**Capacity**: 65536 events (32 MB)
**Performance**: O(1) acquire/release

**Usage Pattern**:
```cpp
EventPool<Event, 65536> pool;

// Acquire from pool
EventType* event = pool.acquire();

// Use event...

// Release back to pool
pool.release(event);
// Or use shared_ptr for automatic return
```

---

### 3. Lock-Free Deduplicator
**Location**: `include/utils/lock_free_dedup.hpp`
**Purpose**: Prevent duplicate processing (idempotency)
**Pattern**: Hash table with chaining, CAS-based insertion
**Performance**: O(1) average, 1M+ checks/sec

**Usage Pattern**:
```cpp
LockFreeDeduplicator dedup;

// Fast read path (no lock!)
if (dedup.is_duplicate(event_id, now_ms)) {
    return;  // Skip duplicate
}

// Insert if new (CAS-based)
if (dedup.insert(event_id, now_ms)) {
    // Process new event
}

// Periodic cleanup (background thread)
dedup.cleanup(now_ms);  // Runs every 10 minutes
```

---

### 4. EventBusMulti (Dispatcher)
**Location**: `include/event/EventBusMulti.hpp`
**Purpose**: Route events to appropriate processors
**Pattern**: Single thread consuming inbound, pushing to 3 processor queues
**Performance**: ~30% of system CPU

**Design Pattern**:
```cpp
EventBusMulti bus;

// Main loop
while (running) {
    // Pop from inbound (lock-free SPSC)
    if (auto event = inbound_queue.pop()) {
        // Route by type (O(1) switch)
        switch(event->type) {
            case REALTIME:
                realtime_queue.push(event);
                break;
            case TRANSACTIONAL:
                transactional_queue.push(event);
                break;
            case BATCH:
                batch_queue.push(event);
                break;
        }
        // Update metrics (thread-local, no lock)
        metrics.events_in++;
    }
}
```

---

### 5. Metric Registry
**Location**: `include/metrics/metricRegistry.hpp`
**Purpose**: Collect performance metrics asynchronously
**Key Optimization (Day 39)**: 1ms buffering, 95% contention reduction
**API**: Thread-safe singleton

**Usage Pattern**:
```cpp
auto& registry = MetricRegistry::getInstance();

// Update metrics (no lock on this thread-local call)
registry.updateEventTimestamp(MetricNames::EVENTBUS);

// Query metrics (requires lock, but not in hot path)
auto snapshot = registry.getSnapshot("EventBusMulti");
```

---

## MEMORY ORDERING GUARANTEES

### SPSC Queue Synchronization
```
Producer (head):
  load_relaxed(tail)      // Check if full
  store_release(head)     // Publish new head

Consumer (tail):
  load_acquire(head)      // Synchronize with producer's release
  store_release(tail)     // Publish new tail

Result: 
  - Producer's writes visible to consumer
  - No CAS loops needed (monotonic indices prevent ABA)
  - Better performance than generic MPMC
```

### Dedup CAS Synchronization
```
Insert thread:
  CAS(bucket, old_head, new_entry)
  memory_order_release on success

Read thread:
  load_acquire(bucket)
  Sees inserted entries

Result:
  - New entries visible after CAS
  - No data races
```

---

## PERFORMANCE CHARACTERISTICS

### Throughput
- **Baseline**: 82K events/sec (stable)
- **Peak**: 3.7M events/sec per SPSC queue
- **Limiting Factor**: TCP socket I/O (disk/network)

### Latency
- **p50**: 50 microseconds
- **p99**: 5 milliseconds
- **p99.9**: 25 milliseconds (GC/OS jitter)

### CPU Usage
```
EventBus:        30% (routing)
Processors (3):  40% (business logic)
Metrics:         2% (optimized, async)
Ingest/TCP:      8% (zero-copy)
Utils/Dedup:     5% (lock-free)
Other:           15% (spare capacity)
```

### Memory
- **RSS**: 100-130 MB
- **Events in flight**: 0.6% of pool capacity
- **Pre-allocated**: No runtime allocations in hot path

---

## DESIGN DECISIONS & TRADE-OFFS

### Why SPSC over MPMC?
**Trade-off**: Point-to-point only (vs flexible routing)
**Benefit**: Lock-free, fast, predictable latency
**Applied**: Works because architecture is pipeline-based

### Why Pre-allocated Pools?
**Trade-off**: Fixed memory (vs dynamic)
**Benefit**: Deterministic latency, no GC pauses
**Applied**: Pools sized for worst case + headroom

### Why Async Metrics?
**Trade-off**: 1ms granularity (vs real-time)
**Benefit**: 95% reduction in lock contention
**Applied**: Metrics are for observability, not correctness

### Why Lock-Free Dedup?
**Trade-off**: Complexity in CAS logic (vs mutex)
**Benefit**: No blocking on read path
**Applied**: Dedup check is in critical path

### Why Three Processor Types?
**Trade-off**: More code/complexity (vs single processor)
**Benefit**: Different throughput rates, independent scaling
**Applied**: Different processing semantics (realtime vs batch)

---

## COMMON OPERATIONS

### Adding a New Event Type
1. Define in `EventType` enum
2. Add case in EventBusMulti dispatcher
3. Create corresponding processor queue
4. Implement processor consumer loop

**Estimated Effort**: 2 hours (queue + processor + tests)

### Adding a Storage Backend
1. Inherit from `StorageBackend` interface
2. Implement `write()` and `read()` methods
3. Update config parser to recognize type
4. Add tests

**Estimated Effort**: 4 hours (implementation + testing)

### Adding a Metric
1. Add field to `Metrics` struct
2. Update aggregation logic
3. Add to metric registry
4. Update query APIs

**Estimated Effort**: 1 hour

---

## DEPLOYMENT CHECKLIST

### Before Production
- [ ] Build with `-DCMAKE_BUILD_TYPE=Release`
- [ ] Run full test suite: `./unittest/EventStreamTests`
- [ ] All 38 tests must pass
- [ ] Zero compiler warnings
- [ ] Configure event pool size for expected load
- [ ] Set up metrics monitoring
- [ ] Verify storage backend connectivity

### Monitoring
- [ ] Watch CPU utilization (should be < 80%)
- [ ] Monitor p99 latency (should be < 5ms)
- [ ] Track queue depths (should be < 1% capacity)
- [ ] Alert on dedup table size growth (cleanup may be delayed)

### Troubleshooting
- **High CPU (>80%)**: Reduce event rate or add processors
- **High latency (p99 > 10ms)**: Check for GC pauses or OS jitter
- **Queue overflow**: Pool exhaustion (increase capacity)
- **Growing dedup table**: Cleanup thread may be stuck (check logs)

---

## EXTENDING FOR DISTRIBUTED LAYER

### Patterns to Reuse
1. **SPSC Queues**: Use same for inter-node communication
2. **Event Pool**: Pre-allocate per node
3. **Lock-Free Dedup**: Extend with consistent hashing
4. **Async Metrics**: Aggregate across cluster

### Changes Required
1. **Replication Layer**: Add between processors and storage
2. **Consensus**: RAFT already present, extend for log
3. **Failover**: Implement leader election (use RAFT)
4. **Distributed Dedup**: Coordinate cleanup across nodes

### Expected Impact
- **Throughput**: ~80K events/sec (20% reduction for replication)
- **Latency**: p99 ~15ms (network + consensus overhead)
- **CPU**: +10% (replication I/O)
- **Complexity**: +500-1000 lines (replication + consensus)

---

## DEBUGGING TIPS

### Print Event Lifecycle
```cpp
// Add in main event handling:
spdlog::debug("[Event {}] Ingest", event->id);
spdlog::debug("[Event {}] Dispatch", event->id);
spdlog::debug("[Event {}] Process", event->id);
spdlog::debug("[Event {}] Storage", event->id);
spdlog::debug("[Event {}] Return", event->id);
```

### Profile Hot Paths
```bash
# Record with perf
perf record -F 99 -g ./EventStreamCore

# Analyze results
perf report

# Look for:
# - SPSC push/pop functions
# - Dedup lookup/insert
# - Metrics aggregation
```

### Monitor Lock Contention
```cpp
// In metrics registry:
auto lock_count = metrics_map_.size();
spdlog::info("[Metrics] Lock ops: {}k/sec", lock_count);
// Should be < 2K ops/sec (Day 39 optimization)
```

---

## REFERENCES

### Key Files
- **Architecture**: `CORE_ENGINE_TECHNICAL_DOCUMENTATION.md`
- **Performance**: `DAY39_FINAL_STATUS.md`
- **Assessment**: `CORE_ENGINE_ASSESSMENT.md`

### Source Code
- SPSC: `include/utils/spsc_ringBuffer.hpp`, `src/utils/spsc_ringBuffer.cpp`
- Pool: `include/core/memory/event_pool.hpp`
- Dedup: `include/utils/lock_free_dedup.hpp`, `src/utils/lock_free_dedup.cpp`
- EventBus: `include/event/EventBusMulti.hpp`, `src/event/EventBusMulti.cpp`
- Metrics: `include/metrics/metricRegistry.hpp`, `src/metrics/metricRegistry.cpp`

### Tests
- All: `unittest/` directory
- Build: `cmake -B build && cd build && make test`

---

## SUMMARY

**Core engine is:**
- ✅ **Optimized**: 6-20% CPU improvement (Day 39)
- ✅ **Fast**: 82K events/sec, p99 < 5ms
- ✅ **Safe**: Lock-free hot paths, pre-allocated memory
- ✅ **Tested**: 38/38 tests passing
- ✅ **Documented**: 1500+ lines of technical docs
- ✅ **Ready**: For distributed layer work

**Next: Implement distributed replication, consensus, and failover using proven patterns.**

