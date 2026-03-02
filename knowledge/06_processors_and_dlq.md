# 06 — Processors & Dead Letter Queue

> **Goal:** Understand each processor's unique logic — SLA enforcement, deduplication, retry, windowed batching — and how the DLQ integrates.

---

## 1. Processor Hierarchy

```
                    EventProcessor (abstract)
                    ├── start() / stop() / process() / name()
                    ├── NUMA affinity (numa_node_)
                    │
          ┌─────────┼──────────────┬──────────────────┐
          ▼         ▼              ▼                   ▼
   RealtimeProc  TransactionalProc  BatchProcessor
   • Alerts      • Dedup (CAS)     • Topic bucketing
   • SLA check   • Retry 3×        • Window flush (5s)
   • Immediate    • Latency hist    • Aggregation stats
```

All three processors share:
- A **ProcessManager** that creates them, runs a `pop→process` loop per thread.
- Access to **StorageEngine** (append-only binary file).
- Access to **DeadLetterQueue** for failures.
- **ProcessedEventStream** observer notification.

---

## 2. RealtimeProcessor — Sub-Millisecond SLA

**File:** `src/core/processor/realtime_processor.cpp`

### 2.1 Processing Flow

```cpp
void RealtimeProcessor::process(const Event& event) {
    auto start_time = high_resolution_clock::now();

    if (!handle(event)) {
        // processing failed → DLQ + notify observers
        dlq_->push(event);
        ProcessedEventStream::notifyDropped(event, name(), "processing_failed");
        return;
    }

    auto elapsed_ms = duration_cast<milliseconds>(now - start_time).count();
    if (elapsed_ms > max_processing_ms_) {
        // SLA BREACH: processing took too long
        emitAlert(AlertLevel::WARNING, fmt::format("SLA breach: {}ms > {}ms", ...));
        dlq_->push(event);
        ProcessedEventStream::notifyDropped(event, name(), "sla_breach");
        return;
    }

    // Success
    storage_->storeEvent(event);
    ProcessedEventStream::notifyProcessed(event, name());
}
```

### 2.2 Alert System

```cpp
void RealtimeProcessor::emitAlert(AlertLevel level, const std::string& message,
                                   const Event& event) {
    Alert alert;
    alert.level = level;
    alert.message = message;
    alert.source = event.topic;
    alert.event_id = event.header.id;
    alert.timestamp_ns = nowNs();
    alert.context = event.body;      // attach raw payload for debugging
    alert_handler_->onAlert(alert);  // polymorphic dispatch
}
```

### 2.3 Domain Logic in `handle()`

```cpp
bool RealtimeProcessor::handle(const Event& event) {
    // Large payload warning
    if (event.body.size() > 1024) {
        emitAlert(WARNING, "Large payload: ... bytes");
        return true;  // still processed
    }

    // Temperature sensor check
    if (event.topic == "sensor/temperature" && !event.body.empty()) {
        uint8_t temp = event.body[0];
        if (temp > 100) emitAlert(CRITICAL, "Temperature critical");
        else if (temp > 80) emitAlert(WARNING, "Temperature warning");
    }

    // Pressure sensor check
    if (event.topic == "sensor/pressure" && !event.body.empty()) {
        if (event.body[0] > 200) emitAlert(EMERGENCY, "Pressure emergency");
    }
    return true;
}
```

**Interview insight:** This is the **Strategy pattern** — domain-specific handling logic can be swapped by changing `alert_handler_` or overriding `handle()`.

---

## 3. TransactionalProcessor — Dedup + Retry

**File:** `src/core/processor/transactional_processor.cpp`

### 3.1 Processing Flow

```
process(event)
  │
  ├─ if paused → drop to DLQ
  │
  ├─ if is_duplicate(event.id) → skip (idempotent)
  │
  ├─ cleanup dedup table if 10s elapsed
  │
  ├─ retry loop (max 3):
  │     handle(event)
  │     └─ if fails → sleep 10ms × attempt, retry
  │
  ├─ if success:
  │     insert into dedup table
  │     store to storage
  │     record latency histogram
  │     notify observers
  │
  └─ if all retries failed:
        push to DLQ
        notify observers (dropped)
```

### 3.2 Lock-Free Deduplication

```cpp
auto now_ms = system_clock::now().time_since_epoch().count();

// Fast lock-free check (no lock, no allocation)
if (dedup_table_.is_duplicate(event.header.id, now_ms)) {
    return;  // already processed, skip silently
}

// ... process event ...

// After success, record in dedup table (CAS insert)
dedup_table_.insert(event.header.id, now_ms);
```

**Why is dedup done here and not in the Dispatcher?**  
The Dispatcher routes all event types. Only transactional events need idempotency guarantees (e.g., a payment shouldn't be processed twice). Putting dedup in the specific processor follows the **single responsibility principle**.

### 3.3 Periodic Dedup Cleanup

```cpp
uint64_t last = last_cleanup_ms_.load(std::memory_order_acquire);
if (last == 0 || now_ms - last > 10000) {  // every 10 seconds
    if (last_cleanup_ms_.compare_exchange_strong(last, now_ms)) {
        dedup_table_.cleanup(now_ms);  // remove entries older than 1 hour
    }
}
```

The CAS on `last_cleanup_ms_` ensures only one thread performs cleanup even if `process()` is called concurrently (though in practice, only one thread calls it).

### 3.4 Exponential Retry

```cpp
for (int attempt = 1; attempt <= max_retries_; ++attempt) {
    if (handle(event)) { success = true; break; }
    if (attempt < max_retries_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
        // Attempt 1: 10ms, Attempt 2: 20ms, Attempt 3: no sleep (give up)
    }
}
```

**Why exponential?** Transient failures (disk I/O, lock contention) often resolve with a short delay. Exponential backoff prevents retry storms under sustained failure.

### 3.5 Latency Histogram

```cpp
if (event.dequeue_time_ns > 0) {
    uint64_t latency_ns = nowNs() - event.dequeue_time_ns;
    latency_hist_.record(latency_ns);
}
```

The histogram uses **log₂ buckets** (see `03_memory_and_numa.md` for `LatencyHistogram` details). This gives O(1) recording and O(N) percentile calculation.

---

## 4. BatchProcessor — Windowed Aggregation

**File:** `src/core/processor/batch_processor.cpp`

### 4.1 Topic Bucketing

```cpp
void BatchProcessor::process(const Event& event) {
    auto now = Clock::now();

    // Find or create bucket for this topic
    auto [it, inserted] = buckets_.try_emplace(event.topic);
    TopicBucket& bucket = it->second;

    // Add event to bucket
    bucket.events.push_back(event);

    // Check if window has elapsed
    if (now - bucket.last_flush_time >= window_) {
        flushBucketLocked(bucket, event.topic);
        bucket.last_flush_time = now;
    }
}
```

### 4.2 TopicBucket Structure

```cpp
struct TopicBucket {
    alignas(64) std::vector<Event> events;    // accumulated events
    alignas(64) std::mutex bucket_mutex;       // per-bucket lock
    Clock::time_point last_flush_time{};       // when last flushed
};
```

**`alignas(64)` on each field** — different topics' buckets can be accessed without false sharing.

### 4.3 Flush Logic

```cpp
void BatchProcessor::flushBucketLocked(TopicBucket& bucket, const std::string& topic) {
    if (bucket.events.empty()) return;

    size_t count = bucket.events.size();
    uint64_t total_bytes = 0;
    uint32_t min_id = UINT32_MAX, max_id = 0;

    for (const auto& evt : bucket.events) {
        total_bytes += evt.body.size();
        min_id = std::min(min_id, evt.header.id);
        max_id = std::max(max_id, evt.header.id);
    }

    // Persist all events
    if (storage_) {
        for (const auto& evt : bucket.events) storage_->storeEvent(evt);
        storage_->flush();
    }

    // Notify observers
    for (const auto& evt : bucket.events) {
        ProcessedEventStream::notifyProcessed(evt, name());
    }

    bucket.events.clear();
}
```

### 4.4 ControlPlane Drop Mode

```cpp
if (drop_events_.load(std::memory_order_acquire)) {
    dlq_->push(event);
    // Also drop events already in the EventBus BATCH queue:
    event_bus_->dropBatchFromQueue(QueueId::BATCH);
    return;
}
```

When the ControlPlane triggers `DROP_BATCH`, the BatchProcessor:
1. Sends the current event to DLQ.
2. Calls `dropBatchFromQueue()` which pops up to 64 events from the BATCH deque and sends them to DLQ.

This rapidly drains the batch queue to reduce system load.

---

## 5. ProcessManager — Thread Orchestration

**File:** `src/core/processor/process_manager.cpp`

### 5.1 Thread-Per-Processor Model

```cpp
void ProcessManager::start() {
    isRunning_ = true;

    realtimeThread_ = std::thread(&ProcessManager::runLoop, this,
        QueueId::REALTIME, realtimeProcessor_.get());
    NUMABinding::bindThread(realtimeThread_, 2);  // pin to core 2

    transactionalThread_ = std::thread(&ProcessManager::runLoop, this,
        QueueId::TRANSACTIONAL, transactionalProcessor_.get());

    batchThread_ = std::thread(&ProcessManager::runLoop, this,
        QueueId::BATCH, batchProcessor_.get());
}
```

### 5.2 Generic Run Loop

```cpp
void ProcessManager::runLoop(const QueueId& qid, EventProcessor* processor) {
    processor->start();

    auto timeout = (qid == QueueId::REALTIME) ? 10ms : 50ms;

    while (isRunning_) {
        auto eventOpt = event_bus.pop(qid, timeout);
        if (!eventOpt) continue;

        try {
            processor->process(*eventOpt.value());
        } catch (const std::exception& e) {
            spdlog::error("Processor {} failed: {}", processor->name(), e.what());
        }
    }

    processor->stop();
}
```

**Key difference:** REALTIME uses a 10ms timeout (low latency), TRANSACTIONAL/BATCH use 50ms (save CPU).

---

## 6. Alert Handler Hierarchy

**File:** `include/eventstream/core/processor/alert_handler.hpp`

```
  AlertHandler (abstract)
  ├── LoggingAlertHandler     — logs to spdlog
  ├── CallbackAlertHandler    — invokes std::function
  ├── CompositeAlertHandler   — dispatches to multiple handlers
  └── NullAlertHandler        — swallows all alerts (testing)
```

**CompositeAlertHandler** is the **Composite pattern**:

```cpp
void onAlert(const Alert& alert) override {
    for (auto& handler : handlers_) {
        try { handler->onAlert(alert); }
        catch (...) { /* log and continue */ }
    }
}
```

Each handler is isolated — one handler throwing doesn't prevent others from executing.

---

## 7. ProcessedEventStream — Observer Pattern

**File:** `include/eventstream/core/processor/processed_event_stream.hpp`

```cpp
class ProcessedEventStream {
    std::vector<ProcessedEventObserverPtr> observers_;
    std::atomic<bool> enabled_{true};

    void notifyProcessed(const Event& event, const char* processor_name) {
        if (!enabled_) return;  // kill-switch
        auto observers_copy = observers_;  // snapshot under lock
        for (auto& obs : observers_copy) {
            obs->onEventProcessed(event, processor_name);
        }
    }
};
```

**Why copy the observer list?**  
If an observer modifies the list during notification (e.g., unsubscribes itself), iterating the original list would be invalidated. The snapshot prevents this.

**Use case:** The C API bridge subscribes to `ProcessedEventStream` to forward events to SDK callbacks (Python/Go).

---

## 8. Storage Engine

**File:** `src/core/storage/storage_engine.cpp`

### 8.1 Binary Format

```
[8B timestamp][1B source_type][4B event_id][4B topic_len][topic_bytes][8B payload_len][payload_bytes]
```

### 8.2 Batched Flush

```cpp
static constexpr size_t FLUSH_BATCH_SIZE = 100;

void storeEvent(const Event& event) {
    std::lock_guard<std::mutex> lock(storageMutex);
    // ... serialize and write ...
    ++eventCount;
    if (eventCount >= FLUSH_BATCH_SIZE) {
        storageFile.flush();
        eventCount = 0;
    }
}
```

**Why batch?** `flush()` is a system call (fsync or fflush) — expensive. Flushing every 100 events amortizes the cost.

---

## 9. Interview Questions

**Q1: What is the difference between the three processors?**  
A: **Realtime:** Immediate processing, SLA-bound (e.g., 5ms), drops events that take too long. **Transactional:** At-least-once semantics with dedup + retry, latency tracking. **Batch:** Collects events into topic-based buckets, flushes on a time window, optimized for throughput.

**Q2: How does the dedup table achieve lock-free reads?**  
A: `is_duplicate()` does a `load(acquire)` on the bucket head and traverses the linked list. No mutex needed because entries are only prepended (CAS) and cleanup only removes old entries — the traversal is safe even under concurrent inserts.

**Q3: What happens if `handle()` throws an exception in RealtimeProcessor?**  
A: The exception propagates to `ProcessManager::runLoop()`, which catches it with `try/catch`, logs the error, and continues the loop. The event is effectively dropped without going to DLQ. (A future improvement would catch inside `process()` and route to DLQ.)

**Q4: Why does BatchProcessor use a per-topic mutex instead of one global mutex?**  
A: Different topics don't interfere with each other. A global mutex would serialize flushes across all topics. Per-topic mutexes allow concurrent flushing of independent topics. However, in the current design, there's only one BatchProcessor thread, so the benefit is more about code clarity and future extensibility.

**Q5: Explain the `notifyProcessed` observer pattern. Why use a snapshot?**  
A: Observers might unsubscribe during notification (or a new observer subscribes). Iterating a vector while it's being modified is undefined behavior. Copying the vector under lock, then notifying the copy without lock, avoids both issues and prevents holding the lock for an unbounded time.

---

## 10. Deep Dive: Exponential Backoff — The Mathematics

### 10.1 TransactionalProcessor Retry

```cpp
for (int retry = 0; retry < max_retries_; ++retry) {
    if (handle(event)) return;  // success
    auto delay_ms = 100 * (1 << retry);  // 100, 200, 400 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}
// All retries exhausted → DLQ
```

**Why exponential (not linear)?** If a downstream dependency is overloaded, linear retry (100ms, 100ms, 100ms) bombards it at the same rate. Exponential retry (100ms, 200ms, 400ms) reduces pressure over time, giving the dependency time to recover.

**Total wait:** $\sum_{i=0}^{2} 100 \cdot 2^i = 100 + 200 + 400 = 700$ ms.

**With jitter (improvement):** Add random jitter to prevent synchronized retries from multiple processors:
```cpp
auto jitter = rand() % (delay_ms / 2);
std::this_thread::sleep_for(milliseconds(delay_ms + jitter));
```

### 10.2 Why 3 Retries Maximum?

After 3 retries (700ms total), if the operation still fails, it's likely a persistent error (bug, corrupt data, unrecoverable downstream failure). More retries waste time. The DLQ preserves the event for manual investigation.

**Retry budget:** At 1M events/sec with 0.1% failure rate = 1000 failures/sec. Each retry: 700ms total → 1000 × 0.7s = 700 thread-seconds. With one thread, this means the processor is blocked 70% of the time. **Retries are expensive** — the max_retries_ is kept low intentionally.

---

## 11. Deep Dive: Deduplication Window and Memory Analysis

### 11.1 Why 1 Hour?

The dedup window (`IDEMPOTENT_WINDOW_MS = 3600000`) must be longer than the maximum redelivery delay. If a network partition lasts 30 minutes and then delivers a duplicate, the dedup map must still have the original entry.

**Trade-off:** Longer window = more memory. Shorter window = risk of missing duplicates.

### 11.2 Memory Consumption

Each `Entry`: 16 bytes (`uint32_t id` + `uint64_t timestamp_ms` + `Entry* next`).

Worst case (all 4096 buckets have entries, 1M unique events in window):
- 1,000,000 entries × 16 bytes = **16 MB**

Best case (few duplicates, low traffic):
- ~10,000 entries × 16 bytes = **160 KB**

### 11.3 Cleanup Amortization

```cpp
// Called by a separate timer (every 60 seconds in admin loop):
void cleanup(uint64_t now_ms) {
    for (size_t i = 0; i < buckets_.size(); ++i) {
        cleanup_bucket_count(i, now_ms);
    }
}
```

Iterating 4096 buckets and unlinking expired entries: ~10-50μs depending on chain lengths. This is negligible compared to the 60-second interval.

---

## 12. Deep Dive: BatchProcessor Topic Bucketing

### 12.1 Why Bucket by Topic?

Without bucketing, a single flush sends events to storage in mixed order (topic A, topic C, topic A, topic B…). Downstream consumers reading by topic must scan all events. With bucketing, events are pre-sorted by topic → downstream reads are sequential → cache-friendly.

### 12.2 Window-Based Flushing

```cpp
struct TopicBucket {
    alignas(64) std::vector<Event> events;
    alignas(64) std::mutex bucket_mutex;
    Clock::time_point last_flush_time{};
};

void process(const Event& event) {
    auto& bucket = getOrCreateBucket(event.topic);
    std::lock_guard lock(bucket.bucket_mutex);
    bucket.events.push_back(event);
    
    if (Clock::now() - bucket.last_flush_time >= window_) {
        flushBucketLocked(bucket, event.topic);
        bucket.last_flush_time = Clock::now();
    }
}
```

**Window semantics:** Flush when time since last flush ≥ 5 seconds. This is a **tumbling window** (non-overlapping). An alternative is a **sliding window** (overlapping) which requires more memory but provides smoother output.

### 12.3 The `alignas(64)` on `events` and `bucket_mutex`

Even though only one BatchProcessor thread exists today, the alignment prevents false sharing between different `TopicBucket` instances in the `unordered_map`. If two buckets are adjacent in memory, their `events` vectors and mutexes could share cache lines → contention if concurrency is added later.

---

## 13. Deep Dive: StorageEngine Zero-Copy Optimization

```cpp
void storeEvent(const Event& event) {
    std::vector<uint8_t> buffer;
    buffer.reserve(estimatedSize);  // single allocation
    
    // Append all fields to buffer (no individual write() calls)
    buffer.insert(buffer.end(), ...timestamp...);
    buffer.insert(buffer.end(), ...source...);
    buffer.insert(buffer.end(), ...topic...);
    buffer.insert(buffer.end(), ...payload...);
    
    // ONE write call instead of 6+
    storageFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
}
```

**Why one write instead of many?** Each `write()` call:
1. Copies data to the kernel's page cache (or stdio buffer).
2. May trigger a context switch if the buffer is full.
3. Updates file position.

One `write()` of 500 bytes is ~3× faster than six `write()` calls of ~80 bytes each due to syscall overhead (~100ns per call on Linux).

**True zero-copy (future improvement):** Use `writev()` (scatter-gather I/O) with `struct iovec` pointing directly to the Event's fields — no intermediate buffer allocation.

---

## 14. Extended Interview Questions

**Q6: How would you implement exactly-once processing?**  
A: The current design provides at-least-once (retry + dedup). For exactly-once: (1) Use a persistent dedup store (e.g., RocksDB) that survives restarts. (2) Write the event and its dedup entry atomically (transaction). (3) On restart, load the dedup store before accepting events. This is what Kafka's "exactly-once semantics" does.

**Q7: What is the difference between at-most-once, at-least-once, and exactly-once?**  
A: **At-most-once:** Fire and forget; event might be lost. (Realtime path — no retry.) **At-least-once:** Retry until acknowledged; event might be duplicated. (Transactional path — retry 3×, dedup catches most duplicates.) **Exactly-once:** Every event processed exactly once. (Requires distributed transaction or idempotent writes + persistent dedup.)

**Q8: Why does RealtimeProcessor have an SLA of 5ms, not lower?**  
A: The 5ms budget includes: queue pop (~10μs) + handle() logic (~100μs) + alert dispatch (~100μs) + storage write (~500μs) + observer notification (~50μs). Total ~760μs typical, leaving ~4.2ms headroom for GC-like pauses, context switches, and NUMA remote access. Tighter SLAs would require `SCHED_FIFO`, memory-locked pages (`mlockall`), and kernel bypass (DPDK).

**Q9: Why does the project use `int max_retries_ = 3` instead of a configurable constant?**  
A: It IS configurable: `setMaxRetries(int retries)` exists. The default is 3 based on the 99th percentile transient failure duration (~500ms). More than 3 retries with exponential backoff would exceed 1.5 seconds — unacceptable for most transactional workloads.

**Q10: How does the DLQ differ from a Kafka dead-letter topic?**  
A: The project's DLQ is in-memory (`std::deque<DLQEntry>`, bounded at 1000). Kafka's DLQ is a persistent topic with replication. The project's DLQ is for debugging/monitoring — it loses data on crash. Kafka's DLQ provides reliable reprocessing. To bridge: `StorageEngine::appendDLQ()` writes to a file, providing basic persistence.
