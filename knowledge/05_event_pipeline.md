# 05 — Event Pipeline (Dispatcher & EventBus)

> **Goal:** Trace an event from network arrival to queue placement. Understand priority routing, pressure adaptation, overflow policies, and retry logic.

---

## 1. Pipeline Overview

```
  [TCP/UDP Ingest]  ──push──►  [MPSC Queue 65k]  ──pop──►  [Dispatcher]
                                                                │
                                                   route() decides QueueId
                                                                │
                                          ┌─────────────────────┼──────────────────────┐
                                          ▼                     ▼                      ▼
                                    [REALTIME]           [TRANSACTIONAL]            [BATCH]
                                   SPSC 16384            deque 131072             deque 32768
                                   DROP_OLD              BLOCK_PRODUCER           DROP_NEW
```

---

## 2. Dispatcher — Single-Thread Event Router

**Files:** `include/eventstream/core/events/dispatcher.hpp`, `src/core/events/dispatcher.cpp`

### 2.1 Inbound: MPSC Queue

Multiple ingest threads push events into a lock-free MPSC queue:

```cpp
bool Dispatcher::tryPush(const EventPtr& evt) {
    if (!inbound_queue_.push(evt)) {
        spdlog::warn("[BACKPRESSURE] Dispatcher MPSC queue full, dropping event");
        return false;
    }
    return true;
}
```

- Capacity: **65536** events (configurable via template parameter).
- If full: event is dropped at the ingest layer (logged as backpressure).

### 2.2 Dispatch Loop

A single thread runs `dispatchLoop()`:

```cpp
void Dispatcher::dispatchLoop() {
    while (running_.load(std::memory_order_acquire)) {
        // 1. Check pipeline state (PAUSED/DRAINING → sleep 100ms)
        if (pipeline_state_) {
            PipelineState state = pipeline_state_->getState();
            if (state == PipelineState::PAUSED || state == PipelineState::DRAINING) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        // 2. Pop from MPSC (non-blocking)
        auto event_opt = inbound_queue_.pop();
        if (!event_opt) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // 3. Route to target queue
        auto queueId = route(event_opt.value());

        // 4. Push to EventBus with exponential backoff retry
        bool pushed = false;
        for (int retry = 0; retry < 3 && !pushed; ++retry) {
            pushed = event_bus_.push(queueId, event_opt.value());
            if (!pushed) {
                auto backoff_us = 10 * (1 << retry);   // 10, 20, 40 μs
                std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
            }
        }

        // 5. If still failed → Dead Letter Queue
        if (!pushed) {
            event_bus_.getDLQ().push(*event_opt.value());
        }
    }
}
```

**Key design decisions:**

| Decision | Rationale |
|----------|-----------|
| Single consumer | MPSC queue requires single consumer; avoids contention on routing |
| 100μs sleep on empty | Prevents busy-waiting while maintaining sub-ms latency |
| 3 retries with exp backoff | Gives EventBus time to drain; doesn't spin indefinitely |
| DLQ fallback | No event is silently lost; DLQ allows post-mortem analysis |

### 2.3 Pipeline State Integration

The Dispatcher checks `PipelineStateManager` (set by the Admin/ControlPlane) on every iteration:

- **RUNNING** → normal dispatch
- **PAUSED** → sleep 100ms, re-check (no events consumed → backpressure builds at MPSC)
- **DRAINING** → same as PAUSED (let processors finish current events)
- **DROPPING / EMERGENCY** → not checked here (handled by ControlPlane actions)

---

## 3. Routing Logic

### 3.1 Priority Promotion via TopicTable

```cpp
EventBusMulti::QueueId Dispatcher::route(const EventPtr& evt) {
    EventPriority priority = EventPriority::MEDIUM;  // default

    // Look up topic in config-driven table
    if (topic_table_ && topic_table_->findTopic(evt->topic, priority)) {
        // Topic found — 'priority' now holds the configured value
    }

    // Promote event if topic priority is higher than event's own
    if (evt->header.priority < priority) {
        evt->header.priority = priority;
    }
```

Example: A `"sensor/temperature"` event arrives with `LOW` priority. But the topic table says `sensor/temperature : CRITICAL`. The event is promoted to `CRITICAL`.

### 3.2 Pressure Adaptation (Downgrade)

```cpp
    adaptToPressure(evt);
```

```cpp
void Dispatcher::adaptToPressure(const EventPtr& evt) {
    auto pressure = event_bus_.getRealtimePressure();

    if (pressure == PressureLevel::CRITICAL || pressure == PressureLevel::HIGH) {
        if (evt->header.priority == EventPriority::HIGH) {
            evt->header.priority = EventPriority::MEDIUM;  // downgrade!
        }
    }
}
```

**Why?** When the REALTIME ring buffer is ≥ 75% full (HIGH) or ≥ 85% full (CRITICAL), we prevent new HIGH-priority events from entering the realtime queue. Only truly CRITICAL events get through. This protects the system from cascading failures.

### 3.3 Queue Selection

```cpp
    if (priority == CRITICAL || priority == HIGH)
        return QueueId::REALTIME;
    else if (priority == MEDIUM || priority == LOW)
        return QueueId::TRANSACTIONAL;
    else  // BATCH
        return QueueId::BATCH;
}
```

---

## 4. EventBusMulti — Three-Queue Fan-Out

**Files:** `include/eventstream/core/events/event_bus.hpp`, `src/core/events/event_bus.cpp`

### 4.1 Queue Configurations

| Queue | Type | Capacity | Overflow Policy | Use Case |
|-------|------|----------|----------------|----------|
| REALTIME | `SpscRingBuffer<EventPtr, 16384>` | 16384 | DROP_OLD | Alerts, SLA-bound events |
| TRANSACTIONAL | `std::deque` + mutex + condvar | 131072 | BLOCK_PRODUCER | Payment, audit, state-change |
| BATCH | `std::deque` + mutex + condvar | 32768 | DROP_NEW | Analytics, aggregation |

### 4.2 REALTIME Queue — Lock-Free Path

```cpp
bool EventBusMulti::push(QueueId q, const EventPtr& evt) {
    if (q == QueueId::REALTIME) {
        // Update pressure level
        size_t used = RealtimeBus_.ringBuffer.size();
        if (used >= 14000)      pressure = CRITICAL;    // ~85%
        else if (used >= 12000) pressure = HIGH;         // ~73%
        else                    pressure = NORMAL;

        if (RealtimeBus_.ringBuffer.push(evt)) {
            return true;
        }

        // Overflow: DROP_OLD policy
        auto old_evt = RealtimeBus_.ringBuffer.pop();  // remove oldest
        dlq_.push(*old_evt.value());                    // send to DLQ
        RealtimeBus_.ringBuffer.push(evt);              // insert new
    }
```

**DROP_OLD rationale:** For realtime alerts, the *newest* event is always more relevant. Old alerts that couldn't be processed in time are already stale.

### 4.3 TRANSACTIONAL Queue — BLOCK_PRODUCER

```cpp
    // BLOCK_PRODUCER: wait up to 100ms for space
    if (!queue->cv.wait_for(lock, 100ms,
        [&]() { return queue->dq.size() < queue->capacity; })) {
        return false;  // timeout — backpressure propagates to Dispatcher
    }
    queue->dq.push_back(evt);
    queue->cv.notify_one();  // wake consumer
```

**Why block?** Transactional events (payments, audits) cannot be dropped. The producer waits for the consumer to create space. If the consumer is too slow (timeout), the Dispatcher retries or sends to DLQ.

### 4.4 BATCH Queue — DROP_NEW

```cpp
    case OverflowPolicy::DROP_NEW:
        metrics.total_events_dropped.fetch_add(1);
        return false;  // reject incoming event
```

**Why drop new?** Batch events are already low-priority. Dropping new arrivals is cheaper than evicting already-buffered events (which may be mid-aggregation).

### 4.5 Pop with NUMA Binding

```cpp
std::optional<EventPtr> EventBusMulti::pop(QueueId q, std::chrono::milliseconds timeout) {
    // Lazy NUMA binding (once per thread)
    static thread_local bool bound = false;
    if (!bound && numa_node_ >= 0) {
        NUMABinding::bindThreadToNUMANode(numa_node_);
        bound = true;
    }

    if (q == QueueId::REALTIME) {
        auto evt_opt = RealtimeBus_.ringBuffer.pop();
        if (evt_opt) {
            evt_opt.value()->dequeue_time_ns = nowNs();  // timestamp for latency
            return evt_opt;
        }
        return std::nullopt;  // no blocking for realtime
    }

    // TRANSACTIONAL / BATCH: blocking pop with timeout
    std::unique_lock<std::mutex> lock(queue->m);
    if (!queue->cv.wait_for(lock, timeout, [queue] { return !queue->dq.empty(); })) {
        return std::nullopt;
    }
    EventPtr event = queue->dq.front();
    queue->dq.pop_front();
    event->dequeue_time_ns = nowNs();
    return event;
}
```

**REALTIME is non-blocking** — the processor thread spins with a 10ms sleep between polls. This guarantees sub-millisecond dequeue latency.

**TRANSACTIONAL/BATCH block** — processor threads sleep on the condition variable until events arrive or timeout. This saves CPU when the system is idle.

---

## 5. Dead Letter Queue (DLQ)

**Files:** `include/eventstream/core/events/dead_letter_queue.hpp`, `src/core/events/dead_letter_queue.cpp`

### 5.1 When Events Enter the DLQ

| Source | Condition |
|--------|-----------|
| Dispatcher | EventBus push failed after 3 retries |
| EventBus (REALTIME) | Ring buffer full + DROP_OLD → old event to DLQ |
| EventBus (BATCH) | DROP_NEW → rejected event to DLQ |
| RealtimeProcessor | Processing failed or SLA breach |
| TransactionalProcessor | Max retries exceeded |
| BatchProcessor | ControlPlane DROP_BATCH action |

### 5.2 DLQ Internals

```cpp
class DeadLetterQueue {
    static constexpr size_t MAX_STORED_EVENTS = 1000;
    std::atomic<size_t> total_dropped_{0};   // total count (never reset)
    std::deque<Event> stored_events_;         // last 1000 for debugging
};
```

The DLQ is a **ring buffer** (oldest evicted when full) + a **global counter**. This keeps memory bounded while providing debugging context.

```cpp
void DeadLetterQueue::push(const Event& e) {
    total_dropped_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    if (stored_events_.size() >= MAX_STORED_EVENTS) {
        stored_events_.pop_front();  // evict oldest
    }
    stored_events_.push_back(e);
}
```

---

## 6. Backpressure Cascade — Full Picture

```
Level 0: HEALTHY
  All queues < 73% full, drop rate < 1%
  → ControlPlane returns RESUME
  → All processors running normally

Level 1: ELEVATED
  REALTIME queue > 73% (12000/16384)
  → RealtimeBus_.pressure = HIGH
  → Dispatcher downgrades HIGH→MEDIUM events

Level 2: DEGRADED
  Drop rate > 1% or queue > 75%
  → ControlPlane returns DROP_BATCH
  → BatchProcessor starts dropping events to DLQ
  → PipelineState = DROPPING

Level 3: CRITICAL
  REALTIME queue > 85% (14000/16384) or drop rate > 2%
  → ControlPlane returns PAUSE_PROCESSOR
  → TransactionalProcessor paused (events to DLQ)
  → PipelineState = PAUSED

Level 4: EMERGENCY
  Drop rate > 10% or queue > 150%
  → ControlPlane returns PUSH_DLQ
  → Both batch and transactional paused
  → PipelineState = EMERGENCY
  → Dispatcher sleeps 100ms per iteration (rate limiting)
```

---

## 7. Interview Questions

**Q1: Why a single dispatch thread instead of multiple?**  
A: The MPSC queue requires a single consumer. A single dispatcher also provides a **sequencing point** — events are ordered by their pop time. With multiple dispatchers, you'd need additional synchronization for ordering guarantees.

**Q2: What happens if the REALTIME SPSC is full and the Dispatcher tries to push a CRITICAL event?**  
A: DROP_OLD policy: the oldest event is popped from the ring buffer, sent to DLQ, and the new event is inserted. This ensures the most recent critical alert always has a slot.

**Q3: Why does TRANSACTIONAL use BLOCK_PRODUCER while BATCH uses DROP_NEW?**  
A: Transactional events represent business logic (payments, audits) that must not be silently dropped. Blocking gives the consumer time to catch up. Batch events are analytics/aggregation where freshness matters less and occasional loss is acceptable.

**Q4: How does backpressure propagate upstream?**  
A: EventBus full → Dispatcher push fails → retry 3× → DLQ + Dispatcher slows down. Meanwhile, ControlPlane detects metrics → sets PipelineState → Dispatcher checks state → sleeps 100ms. This slows ingest → TCP `recv()` blocks → kernel TCP buffer fills → sender gets TCP window=0 → end-to-end backpressure.

**Q5: What is the worst-case latency for a REALTIME event?**  
A: MPSC push (lock-free, ~100ns) + dispatch loop iteration (max 100μs sleep) + SPSC push (lock-free, ~50ns) + SPSC pop in processor (10ms poll interval) ≈ **~10ms worst case** (dominated by the processor poll interval).

---

## 8. Deep Dive: Linux Scheduler Impact on Latency

### 8.1 CFS (Completely Fair Scheduler)

Linux uses CFS by default for normal threads. CFS divides CPU time "fairly" among all runnable threads using a red-black tree sorted by virtual runtime.

**Problem for REALTIME processor:** If many threads are runnable, the RealtimeProcessor thread may wait 1-4ms in the CFS run queue before getting scheduled. This adds to the total event latency.

**Mitigation in the project:** `NUMABinding::bindThread(realtimeThread_, 2)` pins the realtime thread to core 2. If no other CPU-bound threads are on core 2, the thread gets scheduled immediately.

**Further optimization (not in project):** Use `SCHED_FIFO` (real-time scheduling class):
```cpp
struct sched_param param;
param.sched_priority = 99;  // max RT priority
pthread_setschedparam(thread.native_handle(), SCHED_FIFO, &param);
```
`SCHED_FIFO` threads always preempt CFS threads. This guarantees sub-microsecond scheduling latency. **Trade-off:** A buggy FIFO thread can starve all other threads.

### 8.2 Priority Inversion

**Scenario:** Low-priority ingest thread holds mutex (IngestEventPool). High-priority Realtime thread calls `acquireEvent()` and blocks on the mutex. Medium-priority batch thread runs instead.

**Result:** Realtime thread is blocked by a low-priority thread because a medium-priority thread is consuming CPU. This is **priority inversion**.

**Does the project suffer from this?** Only if the Realtime processor thread calls `IngestEventPool::acquireEvent()`. In the current design, only ingest threads call it — the processor works with already-acquired events. So priority inversion is unlikely but possible if the design is extended.

**Solution:** Priority inheritance (PI) mutexes: `pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)`. When a high-priority thread blocks on the mutex, the holder is temporarily boosted to the same priority.

### 8.3 Lock Convoy

**Scenario:** Multiple ingest threads all call `IngestEventPool::acquireEvent()`. Each blocks on the pool mutex. When the mutex is released, the OS wakes one thread. That thread acquires, releases, and the next wakes. This creates a "convoy" — threads take turns one by one.

**Impact:** Even with 8 ingest threads, only one can acquire at a time → pool throughput limited to ~20M acquires/sec.

**Why this is acceptable:** The pool acquire (~50ns) is far cheaper than the event processing (~microseconds). The bottleneck is never the pool.

### 8.4 Thundering Herd

**Scenario:** EventBus `BLOCK_PRODUCER` policy uses `cv.notify_one()` after a consumer pops. But with `notify_all()` instead, all blocked producers would wake up, check the condition, and all but one go back to sleep.

**The project correctly uses `notify_one()`** — only one producer needs to be woken because only one slot became available.

---

## 9. Deep Dive: The Dispatcher's Exponential Backoff

```cpp
while (!pushed && retry_count < MAX_RETRIES) {
    pushed = event_bus_.push(queueId, event_opt.value());
    if (!pushed) {
        retry_count++;
        auto backoff_us = 10 * (1 << (retry_count - 1));  // 10, 20, 40 μs
        std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
    }
}
```

**Why exponential?** If the first push fails, the queue is likely still full after 10μs. Waiting progressively longer avoids wasting CPU on futile retries while giving the consumer time to drain.

**Why cap at 3 retries?** After 3 retries (total wait: 70μs), if the queue is still full, it's a systemic overload — not a transient spike. Continuing to retry would delay all subsequent events. Better to DLQ this one and move on.

**Why not infinite retry?** The dispatcher is single-threaded. While it retries, no other events are being dispatched. A stuck dispatcher creates head-of-line blocking: all ingest threads back up because the MPSC queue fills.

---

## 10. Deep Dive: The 100ms Sleep vs Busy-Wait Trade-off

```cpp
// When pipeline is PAUSED:
if (state == PipelineState::PAUSED || state == PipelineState::DRAINING) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    continue;
}

// When MPSC queue is empty:
if (!event_opt) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    continue;
}
```

**Two different sleeps:**
1. **100ms (pipeline paused):** Long sleep because we're deliberately throttling. No need to check frequently.
2. **100μs (queue empty):** Short sleep to balance latency vs CPU usage.

**Alternatives:**
| Strategy | Latency | CPU Usage |
|----------|---------|-----------|
| Busy-wait (`while (empty) {}`) | ~0 ns | 100% of one core |
| `sleep_for(100μs)` | ≤ 100μs added | ~0.1% |
| `sleep_for(1ms)` | ≤ 1ms added | ~0.01% |
| Condition variable | ~0 ns | ~0% (kernel wakes thread) |

The project uses 100μs sleep as a compromise. A condition variable would be more efficient but requires modifying the lock-free MPSC queue to support blocking (adding a `futex` call on empty).

---

## 11. Extended Interview Questions

**Q6: What is head-of-line blocking in this context?**  
A: If the Dispatcher blocks on pushing to the TRANSACTIONAL queue (BLOCK_PRODUCER, 100ms timeout), all events behind it in the MPSC queue — including CRITICAL events destined for the REALTIME queue — are stuck. The 100ms timeout was specifically added to prevent indefinite blocking. After timeout, the event goes to DLQ and the dispatcher moves on.

**Q7: How would you redesign the dispatcher for lower latency?**  
A: (1) Use an eventfd or condition variable instead of polling the MPSC queue. (2) Use `SCHED_FIFO` for the dispatcher thread. (3) Pre-sort events by priority before routing — process all CRITICAL events first. (4) Use separate MPSC queues per priority level to avoid head-of-line blocking entirely.

**Q8: What happens during a burst of 100K events in 10ms?**  
A: Ingest threads push to MPSC (65536 capacity). If burst > 65536, `tryPush` returns false → events dropped at ingest level. The dispatcher drains at ~10M events/sec (100ns per pop+route+push). 100K events = 10ms to drain — the system handles the burst exactly in real-time. The EventBus queues (131K + 32K + 16K) absorb the shock while processors catch up.

**Q9: Why does the EventBus use `std::deque` for TRANSACTIONAL/BATCH instead of a ring buffer?**  
A: `std::deque` supports efficient push_back and pop_front (amortized O(1)) and can grow dynamically. A ring buffer has fixed capacity — for TRANSACTIONAL (131K capacity), pre-allocating 131K × sizeof(shared_ptr<Event>) = ~1MB is fine, but `deque` also provides `wait_for` compatibility with `condition_variable` (BLOCK_PRODUCER policy) which ring buffers don't natively support.
