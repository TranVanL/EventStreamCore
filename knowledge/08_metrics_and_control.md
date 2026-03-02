# 08 — Metrics & Control Plane

> **Goal:** Understand the monitoring, health evaluation, and autonomous control systems that keep the pipeline stable under load.

---

## 1. Observability Stack

```
┌─────────────────────────────────────────────────────────┐
│                     MetricRegistry (Singleton)           │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ EventBusMulti│  │RealtimeProc  │  │TransactProc  │  │
│  │  .processed  │  │  .processed  │  │  .processed  │  │
│  │  .dropped    │  │  .dropped    │  │  .dropped    │  │
│  │  .queue_depth│  │  .queue_depth│  │  .queue_depth│  │
│  │  .health     │  │  .health     │  │  .health     │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│                                                          │
│  getSnapshots() → { name → MetricSnapshot }              │
└──────────────┬──────────────────────────────────────────┘
               │ every 10 seconds
┌──────────────▼──────────────────────────────────────────┐
│  Admin::loop()                                           │
│  1. Aggregate queue_depth, processed, dropped            │
│  2. ControlPlane::evaluateMetrics()                      │
│  3. ControlPlane::executeDecision() → PipelineState      │
│  4. executeControlAction() → ProcessManager              │
│  5. reportMetrics() → formatted health report            │
└─────────────────────────────────────────────────────────┘
```

---

## 2. Metrics — Atomic Counters

**File:** `include/eventstream/core/metrics/metrics.hpp`

```cpp
struct Metrics {
    std::atomic<uint64_t> total_events_processed{0};
    std::atomic<uint64_t> total_events_dropped{0};
    std::atomic<uint64_t> current_queue_depth{0};
    std::atomic<uint64_t> last_event_timestamp_ms{0};
    std::atomic<uint8_t>  health_status{0};
};
```

Each component (EventBus, RealtimeProcessor, TransactionalProcessor, BatchProcessor) gets its own `Metrics` instance.

**Why atomic?** Metrics are written by processor threads and read by the Admin thread. Atomics avoid data races without mutex overhead. `memory_order_relaxed` is sufficient because we only need eventual consistency for monitoring.

### Metric Snapshot

```cpp
struct MetricSnapshot {
    uint64_t total_events_processed;
    uint64_t total_events_dropped;
    uint64_t current_queue_depth;
    HealthStatus health_status;

    uint64_t get_drop_rate_percent() const {
        uint64_t total = total_events_processed + total_events_dropped;
        return total > 0 ? (total_events_dropped * 100) / total : 0;
    }
};
```

Snapshots are **non-atomic copies** — safe to pass around and compare without worrying about concurrent modification.

---

## 3. MetricRegistry — Global Singleton

**File:** `src/core/metrics/registry.cpp`

### 3.1 Singleton Pattern

```cpp
MetricRegistry& MetricRegistry::getInstance() {
    static MetricRegistry instance;  // Meyers' singleton (C++11 thread-safe)
    return instance;
}
```

**Why Meyers' singleton?** C++11 guarantees that `static` local variable initialization is thread-safe. No double-checked locking needed. Destruction happens at program exit in reverse construction order.

### 3.2 Metric Access Pattern

```cpp
// In EventBus::push() — thread_local to avoid repeated map lookup:
static thread_local auto& metrics = MetricRegistry::getInstance().getMetrics(MetricNames::EVENTBUS);
metrics.total_events_processed.fetch_add(1, std::memory_order_relaxed);

// In MetricRegistry:
Metrics& getMetrics(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto [it, inserted] = metrics_map_.try_emplace(name);
    return it->second;
}
```

**`thread_local`** caches the reference to the `Metrics` object. After the first call, subsequent accesses are a plain pointer dereference — no mutex, no map lookup.

### 3.3 Timestamp Throttling

```cpp
void MetricRegistry::updateEventTimestamp(const std::string& name) {
    thread_local struct { std::string last_name; uint64_t last_update_ns = 0; } buffer;
    constexpr uint64_t UPDATE_INTERVAL_NS = 1'000'000;  // 1ms

    uint64_t now_ns = ...;
    if (buffer.last_name == name && now_ns - buffer.last_update_ns < UPDATE_INTERVAL_NS)
        return;  // skip — updated less than 1ms ago

    std::lock_guard<std::mutex> lock(mtx_);
    it->second.last_event_timestamp_ms.store(now(), std::memory_order_relaxed);
    buffer.last_name = name;
    buffer.last_update_ns = now_ns;
}
```

**Why throttle?** Updating the timestamp on every event (millions/sec) would cause excessive lock contention on the registry mutex. Updating once per millisecond is sufficient for health monitoring while reducing lock pressure by 1000×.

### 3.4 Health Evaluation

```cpp
MetricSnapshot buildSnapshot(Metrics& m, const ControlThresholds& t, uint64_t ts) {
    auto proc  = m.total_events_processed.load(relaxed);
    auto drop  = m.total_events_dropped.load(relaxed);
    auto depth = m.current_queue_depth.load(relaxed);

    double drop_rate = (proc + drop > 0) ? (drop * 100.0) / (proc + drop) : 0.0;

    HealthStatus health = (depth > t.max_queue_depth || drop_rate > t.max_drop_rate)
        ? HealthStatus::UNHEALTHY : HealthStatus::HEALTHY;

    m.health_status.store(static_cast<uint8_t>(health), relaxed);
    return {proc, drop, depth, health};
}
```

---

## 4. ControlThresholds — Tuning Knobs

**File:** `include/eventstream/core/control/thresholds.hpp`

```cpp
struct ControlThresholds {
    uint64_t max_queue_depth         = 5000;    // events in queue
    double   max_drop_rate           = 2.0;     // percent
    uint64_t max_latency_ms          = 100;     // unused currently
    uint64_t min_events_for_evaluation = 1000;  // warmup threshold
    double   recovery_factor         = 0.8;     // unused currently
};
```

These thresholds define when the system transitions between health states.

---

## 5. ControlPlane — 5-Level State Machine

**File:** `src/core/control/control_plane.cpp`

### 5.1 State Transitions

```
                    evaluateMetrics()
                    ┌─────────────┐
                    │             │
        ┌───────────▼─────────────┴──────────────────┐
        │                                             │
        │  ┌──────────┐   queue<75% AND drop<1%      │
        │  │  HEALTHY  │◄─────────────────────────────┤
        │  │  RESUME   │                              │
        │  └────┬─────┘                               │
        │       │ drop>1% OR queue>75%                │
        │  ┌────▼─────┐                               │
        │  │ ELEVATED  │  (logged, no action)         │
        │  └────┬─────┘                               │
        │       │ drop>1% AND was unhealthy before    │
        │  ┌────▼──────┐                              │
        │  │ DEGRADED   │  DROP_BATCH                 │
        │  │            │  BatchProcessor drops events│
        │  └────┬──────┘                              │
        │       │ drop>2% OR queue>100%               │
        │  ┌────▼──────┐                              │
        │  │ CRITICAL   │  PAUSE_PROCESSOR            │
        │  │            │  TransactionalProc paused   │
        │  └────┬──────┘                              │
        │       │ drop>10% OR queue>150%              │
        │  ┌────▼──────┐                              │
        │  │ EMERGENCY  │  PUSH_DLQ                   │
        │  │            │  Batch dropped + TX paused  │
        │  └───────────┘                              │
        └─────────────────────────────────────────────┘
```

### 5.2 Decision Logic

```cpp
EventControlDecision ControlPlane::evaluateMetrics(
    uint64_t queue_depth, uint64_t total_processed,
    uint64_t total_dropped, uint64_t latency_ms
) {
    double drop_rate = (total_events > 0) ? (dropped * 100.0) / total_events : 0.0;
    double queue_util = (max > 0) ? (depth * 100.0) / max : 0.0;

    // Warmup: not enough data yet
    if (total_events < min_events_for_evaluation) {
        if (queue_util >= 100.0)
            return {PAUSE_PROCESSOR, CRITICAL, "Warmup but queue full"};
        return {RESUME, HEALTHY, "Warmup"};
    }

    // EMERGENCY
    if (drop_rate >= 10.0 || queue_util > 150.0)
        return {PUSH_DLQ, CRITICAL, "Emergency"};

    // CRITICAL
    if (drop_rate >= max_drop_rate || queue_util >= 100.0)
        return {PAUSE_PROCESSOR, CRITICAL, "Critical"};

    // DEGRADED (only if previously unhealthy — hysteresis)
    if ((drop_rate >= max_drop_rate * 0.5 || queue_util >= 75.0)
        && previous_state_ != HEALTHY)
        return {DROP_BATCH, DEGRADED, "Degraded"};

    // HEALTHY
    return {RESUME, HEALTHY, "Normal"};
}
```

**Hysteresis:** The DEGRADED state is only entered if the system was *already* unhealthy (`previous_state_ != HEALTHY`). This prevents oscillation between HEALTHY and DEGRADED when metrics hover near the threshold.

### 5.3 Decision Execution

```cpp
void ControlPlane::executeDecision(const EventControlDecision& decision,
                                    PipelineStateManager& state_manager) {
    PipelineState newState;
    switch (decision.action) {
        case RESUME:           newState = RUNNING;   break;
        case DRAIN:            newState = DRAINING;  break;
        case DROP_BATCH:       newState = DROPPING;  break;
        case PUSH_DLQ:         newState = EMERGENCY; break;
        case PAUSE_PROCESSOR:  newState = PAUSED;    break;
    }
    state_manager.setState(newState);
}
```

---

## 6. PipelineStateManager — Atomic State

**File:** `src/core/control/pipeline_state.cpp`

```cpp
class PipelineStateManager {
    std::atomic<PipelineState> state_{PipelineState::RUNNING};

    void setState(PipelineState newState) {
        PipelineState old = getState();
        if (old == newState) return;  // no change
        state_.store(newState, std::memory_order_release);
        spdlog::warn("[PIPELINE] State transition: {} → {}", toString(old), toString(newState));
    }

    PipelineState getState() const {
        return state_.load(std::memory_order_acquire);
    }
};
```

**Why atomic?**  
- Written by Admin thread (every 10s).
- Read by Dispatcher thread (every iteration, ~100μs).
- Mutex would add latency to the hot path. Atomic `load(acquire)` is a single instruction on x86.

---

## 7. Admin Loop — The Monitoring Heartbeat

**File:** `src/core/admin/admin_loop.cpp`

### 7.1 Loop Structure

```cpp
void Admin::loop() {
    int consecutive_unhealthy = 0;

    while (running_) {
        // Sleep 10 seconds (interruptible)
        {
            std::unique_lock<std::mutex> lock(sleep_mutex_);
            sleep_cv_.wait_for(lock, 10s, [this]() { return !running_; });
        }
        if (!running_) break;

        // 1. Collect snapshots from all components
        auto snaps = registry.getSnapshots();

        // 2. Aggregate totals
        uint64_t total_queue = 0, total_processed = 0, total_dropped = 0;
        for (const auto& [name, snap] : snaps) {
            total_queue += snap.current_queue_depth;
            total_processed += snap.total_events_processed;
            total_dropped += snap.total_events_dropped;
        }

        // 3. Evaluate and act
        auto decision = control_plane_->evaluateMetrics(total_queue, total_processed, total_dropped, 0);
        control_plane_->executeDecision(decision, pipeline_state_);
        executeControlAction(decision);

        // 4. Track consecutive unhealthy cycles
        if (decision.reason != HEALTHY) {
            consecutive_unhealthy++;
            if (consecutive_unhealthy >= 3)
                spdlog::error("[Admin] Unhealthy for {} cycles!", consecutive_unhealthy);
        } else {
            if (consecutive_unhealthy > 0)
                spdlog::info("[Admin] Recovered after {} unhealthy cycles", consecutive_unhealthy);
            consecutive_unhealthy = 0;
        }

        // 5. Print dashboard
        reportMetrics(snaps, decision);
    }
}
```

### 7.2 Control Actions

```cpp
void Admin::executeControlAction(const EventControlDecision& decision) {
    switch (decision.action) {
        case PAUSE_PROCESSOR:
            process_manager_.pauseTransactions();
            break;
        case DROP_BATCH:
            process_manager_.dropBatchEvents();
            break;
        case PUSH_DLQ:
            process_manager_.dropBatchEvents();
            process_manager_.pauseTransactions();
            break;
        case RESUME:
            process_manager_.resumeTransactions();
            process_manager_.resumeBatchEvents();
            break;
    }
}
```

### 7.3 Health Report Dashboard

```
╔════════════════════════════════════════════════════════════╗
║              SYSTEM HEALTH REPORT                          ║
╠════════════════════════════════════════════════════════════╣
║ [✓] EventBusMulti        │ Proc:    12345 │ Drop:     0 (  0.0%) │ Q:     5 ║
║ [✓] RealtimeProcessor    │ Proc:     3000 │ Drop:     2 (  0.1%) │ Q:     0 ║
║ [✓] TransactionalProcessor│ Proc:     8000 │ Drop:     5 (  0.1%) │ Q:    12 ║
║ [✓] BatchProcessor       │ Proc:     1345 │ Drop:     0 (  0.0%) │ Q:     0 ║
╠════════════════════════════════════════════════════════════╣
║ Pipeline: RUNNING    │ Decision: RESUME          │ Health: HEALTHY  ║
╠════════════════════════════════════════════════════════════╣
║ AGGREGATE: 4 OK, 0 ALERTS │ Total Q:     17 │ Drop:   0.0%     ║
╚════════════════════════════════════════════════════════════╝
```

---

## 8. Latency Histogram

**File:** `include/eventstream/core/metrics/histogram.hpp`

### 8.1 Log₂ Bucketing

```cpp
static size_t bucketForLatency(uint64_t latency_ns) {
    if (latency_ns <= 1) return 0;
    int msb = 63 - __builtin_clzll(latency_ns);  // position of highest set bit
    return std::min(static_cast<size_t>(msb), NUM_BUCKETS - 1);
}
```

**`__builtin_clzll`** = Count Leading Zeros (long long). For latency = 1000ns:
- Binary: `0000...0001111101000`
- Highest set bit at position 9
- Bucket 9 covers range [512, 1023] ns

This gives **logarithmic resolution** — detailed for low latencies (where microsecond differences matter), coarse for high latencies (where precision is less important).

### 8.2 Percentile Calculation

```cpp
uint64_t calculatePercentile(double percentile) const {
    // Reconstruct samples from bucket counts
    std::vector<uint64_t> samples;
    for (bucket b : all_buckets) {
        for (count times) {
            samples.push_back(bucket_midpoint);
        }
    }
    // Use nth_element for O(N) selection
    size_t idx = (percentile / 100.0) * samples.size();
    std::nth_element(samples.begin(), samples.begin() + idx, samples.end());
    return samples[idx];
}
```

**`std::nth_element`** is O(N) average — much faster than full sort O(N log N) when you only need one percentile.

---

## 9. ControlDecision — Value Object

**File:** `include/eventstream/core/admin/control_decision.hpp`

```cpp
struct EventControlDecision {
    ControlAction action;     // NONE, PAUSE, DROP_BATCH, DRAIN, PUSH_DLQ, RESUME
    FailureState  reason;     // HEALTHY, DEGRADED, CRITICAL
    std::string   details;    // human-readable explanation
    uint64_t timestamp_ms{0};
};
```

Decisions are **immutable value objects** — created by `evaluateMetrics()`, consumed by `executeDecision()` and `executeControlAction()`. No shared state, no thread-safety concerns.

---

## 10. Interview Questions

**Q1: Why evaluate metrics every 10 seconds instead of every event?**  
A: Evaluating per-event would add overhead to the hot path. Metrics are **statistical** — a 10-second window smooths out bursts and gives a reliable signal. If faster reaction is needed, the 10s interval can be reduced.

**Q2: What is hysteresis and why is it important here?**  
A: Hysteresis prevents rapid oscillation between states. Without it, if drop_rate hovers at 1%, the system would flip between HEALTHY and DEGRADED every 10 seconds, causing thrashing. The check `previous_state_ != HEALTHY` ensures we only enter DEGRADED if we were already in a bad state.

**Q3: How does the PipelineState affect the Dispatcher?**  
A: The Dispatcher reads `PipelineState` atomically on every loop iteration. If PAUSED or DRAINING, it sleeps 100ms and re-checks. This effectively rate-limits ingest without modifying the ingest servers. Events accumulate in the MPSC queue (bounded at 65536) and then in kernel TCP buffers, naturally propagating backpressure to senders.

**Q4: What is the `thread_local` metric reference optimization?**  
A: `static thread_local auto& metrics = MetricRegistry::getInstance().getMetrics(name)` caches the reference. The first call locks the registry mutex and does a hash map lookup. Subsequent calls (millions per second) are a plain pointer dereference — zero overhead.

**Q5: Why use `wait_for` with a condition variable for the Admin sleep instead of `sleep_for`?**  
A: `sleep_for` cannot be interrupted — when `stop()` is called, the Admin thread would block for up to 10 seconds. `wait_for` with `sleep_cv_` allows `stop()` to call `sleep_cv_.notify_all()` and wake the Admin immediately for a clean shutdown.

---

## 11. Deep Dive: Control Theory Perspective

### 11.1 PID Controller Analogy

The AdminController is essentially a **bang-bang controller** (on/off) rather than a PID controller:

| Control Type | Description | This Project |
|-------------|-------------|-------------|
| **Proportional (P)** | Response proportional to error magnitude | ❌ Not used — thresholds are binary |
| **Integral (I)** | Accumulate error over time to eliminate steady-state offset | ❌ Not used |
| **Derivative (D)** | React to rate of change of error | ❌ Not used |
| **Bang-Bang** | Binary: above threshold → act, below → don't | ✅ This is what we do |

**Why bang-bang is acceptable here:** The controlled variable (pipeline state) is inherently discrete — you're either accepting events or not. Proportional control (e.g., gradually reducing throughput) would require variable-rate ingest, which adds complexity. The 5-level escalation (HEALTHY → EMERGENCY) approximates proportional response with discrete steps.

**Interview upgrade — proposing PID:** "To evolve this, I'd add proportional response within each level. For example, at ELEVATED state, instead of doing nothing, dynamically adjust the sleep duration in the Dispatcher's tight loop based on queue fullness. This gives smoother degradation."

### 11.2 Oscillation Proof with Hysteresis

**Without hysteresis (naive approach):**
```
Time 0s:  drop_rate = 0.8%   → HEALTHY       (threshold = 1.0%)
Time 10s: drop_rate = 1.2%   → DEGRADED      (above threshold)
Time 20s: drop_rate = 0.9%   → HEALTHY       (below threshold)
Time 30s: drop_rate = 1.1%   → DEGRADED      (above threshold)
→ Oscillation: state changes every 10 seconds!
```

**With hysteresis (project approach):**
```
HEALTHY → DEGRADED transition:  drop_rate > 1.0%
DEGRADED → HEALTHY transition:  drop_rate < 0.5%  (lower threshold)
```

This creates a **dead zone** between 0.5% and 1.0% where no state change occurs. The system stays in its current state. The gap width (0.5%) determines stability — wider gap = more stable but slower reaction.

**Formal guarantee:** Oscillation period T must satisfy:  
$$T > \frac{\text{dead zone width}}{\text{max rate of change of metric}}$$

If the metric can change at most 0.1% per second, and dead zone is 0.5%, oscillation period must be > 5 seconds. Since the evaluation interval is 10 seconds, we're guaranteed no oscillation.

### 11.3 Exponential Weighted Moving Average (EWMA)

The current implementation uses raw instantaneous metrics. A production upgrade would add EWMA smoothing:

```cpp
// Alpha controls responsiveness: higher = more reactive, lower = smoother
// Alpha = 2/(N+1) where N is the number of samples in the "window"
constexpr double ALPHA = 0.2;  // ~9-sample window

double ewma_drop_rate_ = 0.0;

void updateMetrics(double current_drop_rate) {
    ewma_drop_rate_ = ALPHA * current_drop_rate + (1.0 - ALPHA) * ewma_drop_rate_;
}
```

**Why EWMA over simple moving average?**
- SMA requires storing N samples → O(N) memory.
- EWMA requires storing one value → O(1) memory.
- EWMA gives exponentially decaying weight to old samples, naturally forgetting stale data.
- The decay factor `(1 - α)^k` for sample k steps ago:
  - k=0: weight = 1.0
  - k=5: weight = 0.328
  - k=10: weight = 0.107
  - k=20: weight = 0.012 (essentially forgotten)

---

## 12. Deep Dive: `std::nth_element` vs Full Sort

### 12.1 Algorithm Internals

`std::nth_element` uses **Introselect** (hybrid of Quickselect + Median-of-medians):

```
Initial array:  [50, 20, 90, 10, 70, 30, 80, 60, 40]
Want P99: idx = 0.99 * 9 = 8 (last element when sorted)

Quickselect partition around pivot 40:
  [20, 10, 30]  40  [50, 90, 70, 80, 60]
  Now we know: element at idx 8 is in the right partition
  Recurse on right partition...
```

**Complexity comparison for N=10,000 buckets:**

| Algorithm | Average | Worst | Allocations |
|-----------|---------|-------|-------------|
| `std::sort` + index | O(N log N) = ~133K comparisons | O(N log N) | 0 (in-place) |
| `std::nth_element` | O(N) = ~10K comparisons | O(N²) rare | 0 (in-place) |
| `std::partial_sort` (top-k) | O(N log k) | O(N log k) | 0 (in-place) |

For a single percentile, `nth_element` is the optimal choice. For multiple percentiles (P50, P90, P99), call `nth_element` once for P50, then again on the right partition for P90, then again for P99 — total cost is still O(N).

### 12.2 Why Not a Pre-Sorted Structure?

Using `std::set` or `std::priority_queue` to maintain sorted order would cost O(log N) per insertion. With 1M events/sec, that's 1M × log(10K) ≈ 13M comparisons per second just for sorting. The bucket histogram + periodic `nth_element` costs O(N) per 10 seconds = ~10K comparisons. **3 orders of magnitude less work.**

---

## 13. Deep Dive: `condition_variable` and `futex` Internals

### 13.1 From C++ to Kernel

```
std::condition_variable::wait_for(lock, 10s, pred)
  └→ pthread_cond_timedwait()
       └→ futex(FUTEX_WAIT_BITSET, ..., abs_timeout)
            └→ kernel: add thread to futex wait queue, set TASK_INTERRUPTIBLE
                 └→ schedule() — context switch to another thread
```

### 13.2 Wake Path

```
std::condition_variable::notify_all()
  └→ pthread_cond_broadcast()
       └→ futex(FUTEX_WAKE, ..., INT_MAX)
            └→ kernel: wake all threads on this futex's wait queue
                 └→ each thread: TASK_INTERRUPTIBLE → TASK_RUNNING
                      └→ re-acquire mutex, check predicate
```

### 13.3 Why `notify_all` Instead of `notify_one`?

The Admin uses `notify_all()` during shutdown. If there were multiple Admin threads (future scaling), `notify_one` could wake the wrong one — a thread that re-sleeps while the shutting-down thread never wakes. `notify_all` is correct for shutdown signals.

**Cost of `notify_all` with one waiter:** Identical to `notify_one`. The futex wake queue has exactly one entry; the kernel iterates once.

### 13.4 Spurious Wakeups

The predicate `[this]{ return stopped_.load(); }` in `wait_for` protects against spurious wakeups — the kernel may wake a thread for reasons unrelated to `notify`. The lambda is re-checked after every wake:

```cpp
// Internally, wait_for is equivalent to:
while (!pred()) {
    if (cv.wait_until(lock, deadline) == cv_status::timeout)
        return pred();  // One final check after timeout
}
return true;
```

Without the predicate, a spurious wakeup would cause the Admin to run its evaluation loop early — not catastrophic, but incorrect timing.

---

## 14. Deep Dive: Thread-Safe Metric Registry

### 14.1 The Double-Checked Locking Pattern

```cpp
// Thread-safe singleton
static MetricRegistry& getInstance() {
    static MetricRegistry instance;  // C++11 guarantees thread-safe initialization
    return instance;
}
```

This uses **Meyers' Singleton** which relies on the C++11 guarantee that function-local statics are initialized exactly once, even with concurrent calls. The compiler generates a hidden guard variable and uses `__cxa_guard_acquire` (which internally uses a futex) to ensure single initialization.

### 14.2 Read-Write Lock for Metric Lookup

```cpp
mutable std::shared_mutex registry_mutex_;
std::unordered_map<std::string, MetricCounter> metrics_;

MetricCounter& getMetrics(const std::string& name) {
    // Fast path: read lock
    {
        std::shared_lock<std::shared_mutex> rlock(registry_mutex_);
        auto it = metrics_.find(name);
        if (it != metrics_.end()) return it->second;
    }
    // Slow path: write lock (first call only)
    std::unique_lock<std::shared_mutex> wlock(registry_mutex_);
    return metrics_[name];  // Insert if not found
}
```

Combined with `thread_local` caching, the total lock acquisitions over the program lifetime:
- **Without `thread_local`:** 1M events/sec × 3 lookups × 3600 sec = 10.8B lock acquisitions/hour.
- **With `thread_local`:** num_threads × num_unique_names = ~10 lock acquisitions total.

**10 orders of magnitude reduction in synchronization overhead.**

---

## 15. Extended Interview Questions

**Q6: How would you add percentile alerting (e.g., alert if P99 > 10ms)?**  
A: In `evaluateMetrics()`, after reading the histogram, call `calculatePercentile(99)`. If it exceeds 10ms for N consecutive evaluation cycles (to avoid alerting on transient spikes), set a `DEGRADED` state. The N-consecutive-cycles requirement is essentially a simple low-pass filter.

**Q7: The `MetricCounter` uses `atomic<uint64_t>` for counters. Why not `atomic<double>` for rates?**  
A: Rates are derived values (events / time_window), not accumulated values. Computing them requires reading two counters and a time delta — a multi-step operation that can't be atomic. Instead, store raw counts atomically and compute rates during the 10-second evaluation window. `atomic<double>` on x86 is lock-free but has no `fetch_add` — you'd need CAS loops, which are slower than integer atomics.

**Q8: What happens if the Admin thread crashes?**  
A: No monitoring means the pipeline runs in whatever its last state was — potentially PAUSED or DRAINING, which would eventually cause event loss. A production system needs a watchdog: a separate process that monitors the Admin thread's heartbeat (e.g., last evaluation timestamp in shared memory) and restarts it if stale.

**Q9: How would you expose metrics to Prometheus?**  
A: Add an HTTP endpoint (e.g., `/metrics`) that outputs in Prometheus exposition format:
```
# HELP esc_events_total Total events processed
# TYPE esc_events_total counter
esc_events_total{processor="realtime"} 1234567
esc_events_total{processor="transactional"} 987654
# HELP esc_latency_seconds Event processing latency
# TYPE esc_latency_seconds histogram
esc_latency_seconds_bucket{le="0.001"} 50000
esc_latency_seconds_bucket{le="0.01"} 95000
esc_latency_seconds_bucket{le="+Inf"} 100000
```
Prometheus scrapes this endpoint every 15 seconds. The histogram buckets map directly to our log₂ buckets.

**Q10: Why is `ControlAction` an enum class instead of an `int`?**  
A: `enum class` provides type safety — you can't accidentally compare a `ControlAction` with an integer or a different enum. It also scopes the values (`ControlAction::PAUSE` not just `PAUSE`), preventing name collisions. The underlying type is `int` by default, so there's zero runtime overhead.
