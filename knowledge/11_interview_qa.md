# 11 — Interview Questions & Answers

> **Goal:** 120+ questions organised by topic area. Every answer references the actual project code.

---

## Table of Contents

| Section | Topics | Questions |
|---------|--------|-----------|
| A | System Design | Q1–Q10 |
| B | C++ Language | Q11–Q20 |
| C | Concurrency & Multithreading | Q21–Q30 |
| D | Lock-Free Programming | Q31–Q40 |
| E | Memory & NUMA | Q41–Q47 |
| F | Networking | Q48–Q53 |
| G | Metrics & Control | Q54–Q58 |
| H | Patterns & Architecture | Q59–Q65 |
| J | OS Internals & System Calls | Q66–Q70 |
| K | Compiler, Linker & Build System | Q71–Q74 |
| L | Advanced Concurrency Scenarios | Q75–Q80 |
| M | Error Handling & Exception Safety | Q81–Q88 |
| N | Testing, Debugging & Sanitizers | Q89–Q97 |
| O | Performance Profiling & Optimization | Q98–Q106 |
| P | CMake & Build System | Q107–Q112 |
| Q | Security & Robustness | Q113–Q118 |
| R | STL Internals & Smart Pointers | Q119–Q126 |
| S | Trade-offs & "What Would You Change?" | Q127–Q135 |
| T | Production & Deployment Readiness | Q136–Q142 |

---

## A — System Design

### Q1. Describe the high-level architecture.

**A:** EventStreamCore is a high-performance event processing engine with a multi-stage pipeline.

1. **Ingest layer** — TCP and UDP servers receive raw bytes, parse a wire protocol `[4B len][1B pri][2B topic_len][topic][payload]`, and produce `Event` objects.
2. **Dispatch layer** — A dedicated `Dispatcher` thread drains a lock-free MPSC queue and routes each event through a `TopicTable` to determine the final priority.
3. **Processing layer** — Three processor types (`Realtime`, `Transactional`, `Batch`) each run on their own thread via `ProcessManager`.
4. **Storage layer** — `StorageEngine` appends processed events to a binary file.
5. **Cross-cutting** — `MetricRegistry` collects counters, `ControlPlane` implements 5-level backpressure, and `AdminLoop` periodically prints snapshots.

Data flows **ingest → MPSC → dispatcher → EventBus → processors → StorageEngine**.

Thread count: 1 TCP accept + N client handlers + 1 UDP + 1 Dispatcher + 3 Processors + 1 Admin = **8 + N** threads.

---

### Q2. Why three separate processors instead of one?

**A:** Different event categories have different latency, reliability, and throughput requirements:

- **Realtime** — SLA ≤ 5 ms, alerts on breaches, no retry. For monitoring/alerting.
- **Transactional** — Deduplication (CAS map with 1 h window), 3× retry with exponential backoff, exactly-once semantics. For orders/payments.
- **Batch** — Topic bucketing with 5 s flush window, high throughput, no per-event guarantees. For logging/analytics.

One monolithic processor would need to make a latency vs reliability tradeoff for every event. Separation lets each optimise for its own constraints.

---

### Q3. How does the system handle backpressure?

**A:** Five cascading levels (in `ControlPlane`):

| Level | Trigger | Action |
|-------|---------|--------|
| HEALTHY | queue fill < 60% | Normal |
| ELEVATED | fill ≥ 60% | Log warning, downgrade HIGH→MEDIUM |
| DEGRADED | fill ≥ 75% | Route BATCH events to DLQ |
| CRITICAL | fill ≥ 90% | Pause TRANSACTIONAL processor |
| EMERGENCY | fill ≥ 95% | Pause all, DLQ everything non-REALTIME |

The transitions use **hysteresis**: to step down a level, the metric must cross a *lower* threshold than the one that triggered the step up. This prevents oscillation.

End-to-end backpressure reaches the network: when the ingest thread stops reading from the TCP socket, the kernel receive buffer fills, the TCP window advertises 0, and the remote sender pauses. No data is lost — TCP handles it.

---

### Q4. What happens when an event fails processing?

**A:** It enters the Dead-Letter Queue (DLQ):

- `TransactionalProcessor`: up to 3 retries with exponential backoff (100 ms, 200 ms, 400 ms). After exhausting retries → DLQ.
- `EventBus::push()` with `DROP_OLD` → evicted event → DLQ.
- `ControlPlane` in DEGRADED/EMERGENCY → shed events → DLQ.

DLQ entries include the original event, a reason string, and a timestamp. They're stored in a bounded deque (default capacity: 1,000) and can be drained by external tooling.

---

### Q5. How would you scale this system horizontally?

**A:** The current codebase is single-process. To scale:

1. **Multiple instances** behind a load balancer (topic-based sharding). Events with the same topic go to the same instance for dedup correctness.
2. **CAS dedup map** → replace with a distributed dedup (e.g., Redis + TTL) for cross-instance dedup.
3. **Storage** → replace `StorageEngine` (local file) with Kafka/Pulsar.
4. **Metrics** → expose via Prometheus endpoint (the Python SDK adapter already does this).

The C API bridge (`esccore.h`) makes it easy to embed the engine in any language's process.

---

### Q6. Why does the project use a custom wire protocol instead of gRPC/HTTP?

**A:** Minimal overhead. The wire frame is:

```
[4 bytes: total length (big-endian)]
[1 byte: priority]
[2 bytes: topic length]
[N bytes: topic (UTF-8)]
[M bytes: payload]
```

Total overhead: 7 + topic_len bytes. A gRPC frame adds HTTP/2 framing (~9 bytes), protobuf encoding, and TLS overhead. For an ingest path targeting microsecond latency, every byte matters.

The `IngestServer::parseFrame()` function uses `memcpy` + `ntohl` for zero-copy deserialization.

---

### Q7. How does the TopicTable work?

**A:** `TopicTable` is a `shared_mutex`-protected `unordered_map<string, TopicConfig>`.

```cpp
std::optional<TopicConfig> findTopic(const std::string& topic) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = topics_.find(topic);
    if (it != topics_.end()) return it->second;
    return std::nullopt;
}
```

- **Read path** (`findTopic`): `shared_lock` — multiple readers can execute concurrently.
- **Write path** (`registerTopic`, `loadFromFile`): `unique_lock` — exclusive access.

Topics loaded from YAML (`config.yaml`) at startup. Each topic maps to a priority and a target processor type. If a topic is not found, the event keeps its original priority.

---

### Q8. Walk through the lifecycle of a single TCP event.

**A:**

1. `IngestServer::acceptLoop()` accepts a new connection → spawns a `std::thread` for `handleClient()`.
2. `handleClient()` calls `recv(fd, &len, 4, MSG_WAITALL)` to read the 4-byte length prefix.
3. `parseFrame()` extracts priority, topic, payload.
4. `IngestEventPool::acquireEvent()` gets a pooled `shared_ptr<Event>` (or allocates a new one).
5. `EventFactory::createEvent()` fills the event (ID, CRC-32, timestamp).
6. The ingest thread pushes the event into the MPSC queue: `mpsc_.push(event)`.
7. `Dispatcher::dispatchLoop()` pops from the MPSC queue.
8. Dispatcher calls `topicTable_->findTopic(topic)` → promotes/demotes priority.
9. Dispatcher calls `eventBus_->push(event, priority)` → event lands in the correct queue (REALTIME / TRANSACTIONAL / BATCH).
10. `ProcessManager::runLoop()` pops from the `EventBus` queue.
11. `RealtimeProcessor::process(event)` → checks SLA, fires alert if > 5 ms.
12. `ProcessedEventStream::notifyProcessed()` → observers are called.
13. `StorageEngine::store(event)` → appended to binary file.

---

### Q9. How do you test this system?

**A:** Three levels:

- **Unit tests** (`unittest/`): Google Test. Tests for SPSC push/pop, EventBus routing, TopicTable CRUD, EventFactory CRC integrity, MetricRegistry snapshot.
- **Benchmarks** (`benchmark/`): Measures SPSC throughput (events/sec), MPSC contention under N producers, end-to-end latency.
- **Integration** via C API: Python SDK can drive the engine end-to-end (`engine.submit()` → callback fires).

---

### Q10. What are the single points of failure?

**A:**

- **Dispatcher thread** — if it crashes, events accumulate in the MPSC queue until full, then ingest threads block (BLOCK_PRODUCER) or drop.
- **StorageEngine** — if disk fills, writes fail. No fallback storage.
- **MetricRegistry** — singleton means a bug here affects all monitoring.

Mitigations: Dispatcher has `try/catch` around `dispatch()`, StorageEngine logs errors and continues, MetricRegistry uses only atomic operations (no locks).

---

## B — C++ Language

### Q11. Why C++17 specifically?

**A:** Several features used throughout require C++17:

| Feature | Used In |
|---------|---------|
| `std::optional` | TopicTable::findTopic(), AppConfig fields |
| `std::string_view` | parseFrame(), topic lookups |
| `if constexpr` | EventPool SFINAE path selection |
| Structured bindings | Range-based iteration over maps |
| `std::shared_mutex` | TopicTable reader-writer lock |
| `inline` variables | Header-only constants |
| `[[nodiscard]]` | Factory methods, pool acquire |

C++14 would require `boost::optional`, manual SFINAE, and `shared_timed_mutex` (less efficient).

---

### Q12. Explain `std::optional` usage in the project.

**A:** `TopicTable::findTopic()` returns `std::optional<TopicConfig>`:

```cpp
std::optional<TopicConfig> findTopic(const std::string& topic) const;
```

This is better than:
- Returning a pointer (nullable, but caller might forget to check).
- Throwing an exception (not exceptional — missing topic is expected).
- Returning a sentinel `TopicConfig` (how to represent "not found"?).

`std::optional` makes the "might not exist" explicit in the type system.

---

### Q13. What is `string_view` and why use it?

**A:** `std::string_view` is a non-owning reference to a contiguous character sequence. It stores a pointer and a length — no allocation.

Used in `parseFrame()` to reference the topic bytes inside the receive buffer without copying:

```cpp
std::string_view topic_view(buffer + 7, topic_len);
```

Cost: 0 allocations, 0 copies. A `std::string` would allocate heap memory for every incoming frame.

**Danger:** The `string_view` is only valid as long as the buffer exists. In the project, the buffer outlives the `string_view` because both live in the same function scope.

---

### Q14. Explain `if constexpr` in EventPool.

**A:**

```cpp
if constexpr (has_pool_index<EventType>(0)) {
    // Fast path: use pool_index_ field directly
    size_t idx = obj->pool_index_;
    slots_[idx].store(obj, std::memory_order_release);
} else {
    // Slow path: hash map lookup
    auto it = index_map_.find(obj);
    ...
}
```

`if constexpr` evaluates the condition at **compile time**. The false branch is not even compiled — so it doesn't need to be valid code for that template instantiation. This replaces old-style SFINAE tag dispatch with much cleaner syntax.

---

### Q15. What is SFINAE and where is it used?

**A:** "Substitution Failure Is Not An Error." When the compiler tries to instantiate a template and a substitution fails, it silently removes that overload instead of emitting an error.

In `EventPool`:

```cpp
template<typename T>
static constexpr auto has_pool_index(int)
    -> decltype(std::declval<T>().pool_index_, std::true_type{})
{ return {}; }

template<typename T>
static constexpr std::false_type has_pool_index(...)
{ return {}; }
```

If `T` has `pool_index_`, the first overload is valid and preferred (exact match for `int`). If not, substitution fails silently, and the `...` overload is chosen.

---

### Q16. Explain `std::shared_mutex` vs `std::mutex`.

**A:** `std::shared_mutex` supports two lock modes:

- `std::shared_lock` — multiple threads can hold simultaneously (readers).
- `std::unique_lock` — only one thread, excludes all readers (writer).

Used in `TopicTable`: reads (`findTopic`) are far more frequent than writes (`registerTopic`). With a plain `mutex`, concurrent reads would serialize unnecessarily.

**Tradeoff:** `shared_mutex` has higher per-operation overhead than `mutex` due to internal reference counting. Only beneficial when reads outnumber writes by 10:1 or more — which is the case here (millions of events per few config changes).

---

### Q17. What does `alignas(64)` do?

**A:** Forces the compiler to align the object on a 64-byte boundary (cache line size on x86-64).

```cpp
struct alignas(64) HighPerformanceEvent { ... };
```

**Why?** Prevents **false sharing**. If two threads access different atomic variables that happen to share the same 64-byte cache line, every write by one thread invalidates the other's cache — ping-ponging the line between cores. `alignas(64)` ensures each object starts on its own cache line.

Used in: `HighPerformanceEvent`, SPSC ring buffer's `head_` and `tail_` counters.

---

### Q18. Explain move semantics in the project.

**A:** `EventFactory::createEvent()` takes `std::vector<uint8_t>&& payload` and `std::string&& topic`:

```cpp
Event e;
e.payload = std::move(payload);
e.topic = std::move(topic);
```

`std::move` casts the parameter to an rvalue reference, enabling the move constructor which transfers ownership of heap memory. For a 10 KB payload, this avoids a 10 KB copy.

**Rule of thumb in the project:** Large data (payloads, metadata maps) is always moved. Small data (priority enum, event ID) is copied — moving an `int` is no faster than copying.

---

### Q19. What is `std::call_once` and where is it used?

**A:** Guarantees a callable is invoked exactly once, even if multiple threads call it simultaneously.

```cpp
static std::once_flag flag;
std::call_once(flag, [&] {
    pool = std::make_unique<EventPool<Event, 1024>>();
});
```

Used in `IngestEventPool` for lazy initialization of the pool singleton. It's equivalent to a double-checked locking pattern but without the subtleties of memory ordering — the standard library handles it.

---

### Q20. Why `enum class` over plain `enum`?

**A:**

```cpp
enum class EventPriority : int {
    LOW = 0, MEDIUM = 1, HIGH = 2, CRITICAL = 3
};
```

Benefits:
1. **Scoped** — `EventPriority::LOW`, not just `LOW`. No namespace pollution.
2. **Strongly typed** — can't accidentally compare `EventPriority` to `int` without a cast.
3. **Explicit underlying type** — `: int` guarantees the size and makes serialization portable.

---

## C — Concurrency & Multithreading

### Q21. List all threads and their responsibilities.

**A:**

| Thread | Spawned By | Responsibility |
|--------|-----------|----------------|
| TCP accept | IngestServer::start() | Accept new connections |
| TCP client ×N | handleClient() per connection | Read frames, push to MPSC |
| UDP listener | IngestServer::start() | recvfrom, push to MPSC |
| Dispatcher | Dispatcher::start() | Pop MPSC, route to EventBus |
| RealtimeProcessor | ProcessManager::start() | Pop REALTIME queue, process |
| TransactionalProcessor | ProcessManager::start() | Pop TRANSACTIONAL queue, process |
| BatchProcessor | ProcessManager::start() | Pop BATCH queue, flush window |
| AdminLoop | AdminLoop::start() | Print metrics every 10 s |

---

### Q22. How does graceful shutdown work?

**A:** In `main.cpp`:

1. Signal handler sets `std::atomic<bool> g_running = false`.
2. `main()` was blocked on `g_running.wait(true)` (or condition variable).
3. On wake, it calls `stop()` on each component **in reverse order**:
   - AdminLoop::stop()
   - ProcessManager::stop() — joins processor threads
   - Dispatcher::stop() — joins dispatcher thread
   - IngestServer::stop() — closes sockets, joins accept + client threads
4. Each `stop()` sets its own `running_ = false` and does `thread_.join()`.

**Reverse order** ensures consumers stop after producers — otherwise producers would push to a stopped consumer's queue.

**Async-signal safety:** The signal handler only writes to an atomic. It does NOT call `spdlog` or allocate memory — both are unsafe in signal context.

---

### Q23. Why is the signal handler async-signal-safe?

**A:** The POSIX standard restricts what functions can be called inside a signal handler. Functions like `malloc()`, `printf()`, and `spdlog::info()` use internal locks. If the signal interrupts a thread that already holds that lock → deadlock.

The project's handler:

```cpp
void signalHandler(int sig) {
    g_running.store(false, std::memory_order_relaxed);
}
```

`std::atomic::store` with relaxed ordering is **lock-free** on x86-64, making it async-signal-safe.

---

### Q24. Explain `std::condition_variable` usage.

**A:** `EventBus::push()` with `BLOCK_PRODUCER` policy:

```cpp
std::unique_lock<std::mutex> lock(queue.mutex_);
queue.cv_.wait_for(lock, 100ms, [&] {
    return queue.size() < queue.capacity_;
});
```

1. The mutex protects the queue's internal state.
2. `wait_for` atomically releases the lock and blocks the thread.
3. When a consumer pops an element, it calls `queue.cv_.notify_one()`.
4. The producer wakes, re-acquires the lock, and checks the predicate.
5. The 100 ms timeout prevents indefinite blocking (liveness guarantee).

**Spurious wakeups:** The predicate lambda handles them — if the queue is still full, it goes back to sleep.

---

### Q25. What is a data race? Does this project have any?

**A:** A data race occurs when two threads access the same memory location, at least one is a write, and there is no synchronization.

The project avoids data races via:
- **Atomics** for shared counters (`std::atomic<uint64_t>`).
- **Mutexes** for complex shared state (TopicTable, EventBus deque queues).
- **Lock-free queues** for hot paths (SPSC, MPSC) — correct memory ordering instead of locks.

Potential concern: `ProcessedEventStream::observers_` vector is protected by mutex only on subscribe/unsubscribe. The notification path copies the vector under lock → reads the copy without lock. Safe because each copy is thread-local.

---

### Q26. What is `thread_local` and where is it used?

**A:** `thread_local` gives each thread its own copy of a variable.

Used in MetricRegistry for caching:

```cpp
thread_local MetricSnapshot cached_snapshot;
thread_local steady_clock::time_point last_fetch;
```

Each thread caches the last snapshot for 100 ms. This avoids lock contention on the registry when many threads read metrics simultaneously.

**Lifetime:** Created when the thread first accesses it, destroyed when the thread exits.

---

### Q27. Explain the difference between `std::mutex` and `std::atomic`.

**A:**

| Aspect | `std::mutex` | `std::atomic` |
|--------|-------------|---------------|
| Granularity | Protects a code region | Protects a single variable |
| Blocking | Yes (OS scheduler) | No (busy-wait or lock-free) |
| Overhead | Higher (syscall if contended) | Lower (single instruction) |
| Use case | Complex invariants | Simple counters, flags |

The project uses `std::atomic` for:
- `running_` flags (bool)
- Metric counters (uint64_t)
- SPSC head/tail indexes
- Dedup CAS map entries

And `std::mutex` for:
- TopicTable (map + iteration)
- EventBus deque queues (multi-step push/pop)
- StorageEngine (file I/O)

---

### Q28. What happens if a processor thread throws an exception?

**A:** In `ProcessManager::runLoop()`:

```cpp
try {
    processor->process(*event);
} catch (const std::exception& e) {
    spdlog::error("Processor exception: {}", e.what());
    // Event goes to DLQ
}
```

The exception is caught, logged, and the event is sent to the DLQ. The processor thread continues running. An uncaught exception in a `std::thread` calls `std::terminate()` — so catching is essential.

---

### Q29. Why `std::shared_ptr` for events instead of `unique_ptr`?

**A:** An event may be referenced by:
1. The ingest pool (for recycling).
2. The MPSC queue.
3. The EventBus queue.
4. The processor.
5. The observer callbacks.

With `unique_ptr`, ownership would need to be explicitly transferred at each stage. `shared_ptr` allows multiple non-owning readers without complex ownership choreography. The reference count is atomic, so it's thread-safe.

**Tradeoff:** `shared_ptr` has higher overhead (atomic increment/decrement on every copy). For the hot SPSC path, events are moved (not copied), keeping refcount operations minimal.

---

### Q30. What is `std::memory_order_seq_cst` and when should you use it?

**A:** The strongest ordering: all threads see all `seq_cst` operations in the same total order. It's the default for `std::atomic` operations.

In the project, `seq_cst` is **avoided** on hot paths. Instead:
- SPSC uses `acquire/release` (producer-consumer pattern).
- MPSC uses `acq_rel` exchange.
- Metric counters use `relaxed` (no ordering needed, just atomicity).

`seq_cst` is used only for infrequent operations like `running_` flag checks, where correctness is more important than nanoseconds.

---

## D — Lock-Free Programming

### Q31. Explain the SPSC ring buffer.

**A:** A Single-Producer Single-Consumer lock-free queue.

```
Data:   [  slot 0  |  slot 1  |  ...  |  slot 16383  ]
Head:   write position (only producer modifies)
Tail:   read position (only consumer modifies)
```

**Push (producer):**
1. Read `head_` (local, no atomic needed).
2. Read `tail_` with `acquire` (see consumer's latest position).
3. If `(head_ - tail_) == capacity_` → full, return false.
4. Write data to `buffer_[head_ % capacity_]`.
5. Increment `head_` with `release` (makes data visible to consumer).

**Pop (consumer):**
1. Read `tail_` (local).
2. Read `head_` with `acquire` (see producer's latest position).
3. If `head_ == tail_` → empty, return false.
4. Read data from `buffer_[tail_ % capacity_]`.
5. Increment `tail_` with `release` (makes slot available to producer).

**Why lock-free?** No mutex, no syscall, no blocking. Just atomic loads and stores. On x86-64, `acquire` = plain load, `release` = plain store (TSO gives acquire/release for free).

---

### Q32. What is the ABA problem? Does SPSC have it?

**A:** ABA: Thread 1 reads value A, gets preempted. Thread 2 changes A→B→A. Thread 1 resumes, sees A, and incorrectly assumes nothing changed.

**SPSC does NOT have ABA** because:
- Head and tail are monotonically increasing counters (never wrap — `uint64_t` overflow at 2^64 events is practically impossible).
- Only one thread writes each counter.
- CAS is not used — just load/store.

**The CAS dedup map DOES have a potential ABA risk:** a slot could be freed and reused with the same event ID hash. The project mitigates this with a 1-hour expiry window and 4096 buckets — the probability of ABA is negligible.

---

### Q33. Explain the Vyukov MPSC queue.

**A:** Multi-Producer Single-Consumer. Many ingest threads push; one dispatcher thread pops.

**Push (any producer):**
```cpp
node->next.store(nullptr, relaxed);
Node* prev = head_.exchange(node, acq_rel);  // atomic swap
prev->next.store(node, release);
```

1. `exchange` atomically swaps `head_` with the new node and returns the old head.
2. The old head's `next` is set to the new node.
3. This is wait-free for producers — a single atomic exchange, no retry loop.

**Pop (single consumer):**
```cpp
Node* tail = tail_;
Node* next = tail->next.load(acquire);
if (next) {
    tail_ = next;  // advance tail
    return next->data;
}
return nullptr;
```

**Why `acq_rel` on exchange?** The `release` part publishes the new node's data. The `acquire` part ensures the producer sees any prior writes to `prev`.

---

### Q34. Compare SPSC, MPSC, and mutex-protected deque.

**A:**

| | SPSC | MPSC | Mutex + deque |
|---|------|------|--------------|
| Producers | 1 | N | N |
| Consumers | 1 | 1 | N |
| Blocking | No | No | Yes |
| Bounded | Yes (16384) | Yes (65536) | Yes (configurable) |
| Overhead | ~5 ns | ~20 ns | ~100 ns |
| Used for | Realtime queue | Ingest→Dispatch | Transactional/Batch |

SPSC is fastest because it needs only `load/store` (no CAS, no exchange). MPSC needs one `exchange` per push. Mutex + deque is simplest but serializes all access.

---

### Q35. What is `std::memory_order_acquire`?

**A:** A load with `acquire` ordering guarantees that no reads or writes in the current thread can be reordered *before* it.

In SPSC:
```cpp
size_t current_tail = tail_.load(std::memory_order_acquire);
```

This ensures the producer sees the consumer's latest writes (the freed slot data) before reading `tail_`. Without `acquire`, the CPU could reorder the `tail_` read *after* accessing the buffer slot — reading stale data.

---

### Q36. What is `std::memory_order_release`?

**A:** A store with `release` ordering guarantees that no reads or writes in the current thread can be reordered *after* it.

In SPSC:
```cpp
buffer_[idx] = item;
head_.store(new_head, std::memory_order_release);
```

`release` ensures the data write to `buffer_[idx]` is visible to any thread that subsequently does an `acquire` load of `head_`. It acts as a "publish" barrier.

---

### Q37. What is `std::memory_order_relaxed`?

**A:** Only guarantees atomicity — no ordering with respect to other operations.

Used for metric counters:
```cpp
event_count_.fetch_add(1, std::memory_order_relaxed);
```

We don't care in what order other threads see the increment — we only need the counter to be accurate over time. `relaxed` is the cheapest ordering.

---

### Q38. What does `std::atomic::exchange` do in the MPSC queue?

**A:** Atomically replaces the stored value with a new value and returns the old value. It's a single instruction on x86 (`XCHG`).

```cpp
Node* prev = head_.exchange(node, std::memory_order_acq_rel);
```

This is the key to making MPSC push **wait-free**: every producer succeeds in exactly one atomic operation, regardless of contention.

Compare with CAS (`compare_exchange`), which can fail and retry in a loop — `exchange` never fails.

---

### Q39. What does `compare_exchange_strong` do in the dedup map?

**A:** Atomically: if the current value equals `expected`, replace it with `desired` and return `true`. Otherwise, set `expected` to the current value and return `false`.

```cpp
uint64_t expected = 0;  // empty slot
if (bucket.compare_exchange_strong(expected, event_id,
        std::memory_order_acq_rel)) {
    // Successfully claimed the slot
}
```

The CAS loop in the dedup map handles collisions: if another thread claimed the slot first, `expected` is updated and the thread tries the next bucket (linear probing).

---

### Q40. What is a memory barrier?

**A:** An instruction that prevents the CPU from reordering memory operations across it.

On x86-64:
- `acquire` = plain load (TSO provides it for free).
- `release` = plain store (TSO provides it for free).
- `seq_cst` = store + `MFENCE` instruction.

On ARM/RISC-V, explicit barrier instructions (`DMB`, `FENCE`) are needed for acquire/release.

The project targets x86-64, so `acquire` and `release` compile to ordinary instructions — the lock-free code has **zero barrier overhead**.

---

## E — Memory & NUMA

### Q41. What is NUMA and why does it matter?

**A:** Non-Uniform Memory Access. In multi-socket servers, each CPU socket has "local" memory. Accessing remote memory (attached to the other socket) takes 1.5–2× longer.

`NUMAConfig` in the project:
```cpp
void bindThreadToCPU(int cpu_id);
void* allocateOnNode(size_t size, int node);
```

The dispatcher thread and its MPSC queue should be on the same NUMA node. Otherwise, every `push` and `pop` crosses the interconnect.

---

### Q42. Explain `bindThreadToCPU`.

**A:**

```cpp
void bindThreadToCPU(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
```

Pins the calling thread to a specific CPU core. Benefits:
1. Cache warmth — the thread always uses the same L1/L2 cache.
2. NUMA locality — if the core is on node 0, thread accesses node 0 memory fastest.
3. Deterministic latency — no migration overhead.

---

### Q43. Explain `allocateOnNode`.

**A:**

```cpp
void* allocateOnNode(size_t size, int node) {
    void* ptr = numa_alloc_onnode(size, node);
    // OR: mmap + mbind with MPOL_BIND
    return ptr;
}
```

Allocates memory on a specific NUMA node. Used for the SPSC buffer and event pool — placing the buffer on the same node as the consumer thread.

---

### Q44. What is false sharing?

**A:** When two variables share the same 64-byte cache line, writes by one thread invalidate the other thread's cache — even though they're accessing different variables.

The project uses `alignas(64)` on hot structures:

```cpp
alignas(64) std::atomic<size_t> head_;  // Producer-only
alignas(64) std::atomic<size_t> tail_;  // Consumer-only
```

This guarantees `head_` and `tail_` are on different cache lines. Without it, every SPSC push would invalidate the consumer's `tail_` cache line and vice versa.

---

### Q45. Explain the IngestEventPool custom deleter.

**A:**

```cpp
auto deleter = [](Event* e) {
    e->reset();                     // Clear payload, keep allocated memory
    getPool().push(std::unique_ptr<Event>(e));  // Return to pool
};
return std::shared_ptr<Event>(raw, deleter);
```

When the last `shared_ptr` copy is destroyed, instead of `delete`, the custom deleter resets the event and pushes it back to the pool. This gives O(1) "allocation" (pop from pool) and O(1) "deallocation" (push to pool).

---

### Q46. What is the EventPool's free-list stack?

**A:** A pre-allocated array of slots plus a stack of free indices:

```
Slots:  [Event0] [Event1] [Event2] ... [Event1023]
Free:   [1023, 1022, ..., 1, 0]  ← stack
```

`acquire()`: Pop index from stack → return `&slots_[index]`.
`release()`: Push index back to stack.

Both operations are O(1) with no allocation. The pool is fixed-size (`Capacity` template parameter).

---

### Q47. How does `HighPerformanceEvent` minimise cache misses?

**A:**

```cpp
struct alignas(64) HighPerformanceEvent {
    uint64_t event_id;          // 8
    uint64_t timestamp;         // 8
    EventPriority priority;     // 4
    uint32_t crc32;             // 4
    char topic[128];            // 128
    uint8_t payload[400];       // 400
    uint16_t topic_len;         // 2
    uint16_t payload_len;       // 2
    size_t pool_index_;         // 8
};                              // Total: 564 → padded to 576 (9 × 64)
```

**Inline storage:** Topic and payload are stored in the struct itself (no heap pointer). This eliminates pointer chasing — all data is contiguous in memory, prefetchable by the CPU.

**Tradeoff:** Maximum topic is 128 bytes, maximum payload is 400 bytes. Larger events would need the regular `Event` type with heap-allocated vectors.

---

## F — Networking

### Q48. Explain the TCP server architecture.

**A:**

1. `socket(AF_INET, SOCK_STREAM, 0)` → create TCP socket.
2. `setsockopt(SO_REUSEADDR)` → allow quick restart.
3. `bind()` + `listen()`.
4. `acceptLoop()` on dedicated thread:
   - `accept()` → returns new client `fd`.
   - Spawns `std::thread(handleClient, fd)`.
5. `handleClient()`:
   - Loop: `recv(fd, buf, len, MSG_WAITALL)`.
   - Parse frame → create event → push to MPSC.
   - On error/disconnect → `close(fd)`.

**Thread-per-connection model.** Simple but doesn't scale to 10K+ connections. For that, you'd use `epoll` (Linux) or `io_uring`.

---

### Q49. What is `MSG_WAITALL`?

**A:** A `recv()` flag that blocks until the full requested number of bytes is received (or an error/disconnect occurs).

Without it, `recv(fd, buf, 4, 0)` might return 2 bytes (partial read). The application would need to loop and reassemble.

With `MSG_WAITALL`, `recv(fd, &len_buf, 4, MSG_WAITALL)` blocks until all 4 bytes arrive. Simplifies the parsing code significantly.

---

### Q50. How does the UDP server differ?

**A:**

- Single thread (no accept loop — UDP is connectionless).
- `recvfrom()` reads one datagram at a time.
- Each datagram is one complete event frame.
- No `MSG_WAITALL` needed — UDP datagrams are delivered whole or not at all.
- No guaranteed ordering or delivery.

Used for "fire-and-forget" telemetry where occasional loss is acceptable.

---

### Q51. Explain the CRC-32 implementation.

**A:** Table-based CRC-32 (same polynomial as Ethernet, ZIP, PNG):

```cpp
uint32_t table[256];  // precomputed

static void initTable() {
    for (int i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        table[i] = crc;
    }
}

uint32_t calculateCRC32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}
```

**Why?** Detect payload corruption during transmission. CRC-32 catches all single-bit errors and most multi-bit errors. O(n) with no allocation.

---

### Q52. What is `ntohl` and why is it needed?

**A:** `ntohl` converts a 32-bit integer from **network byte order** (big-endian) to **host byte order** (little-endian on x86).

```cpp
uint32_t len;
memcpy(&len, buf, 4);
len = ntohl(len);
```

Without `ntohl`, a length of 256 (0x00000100 in big-endian) would be misread as 16777216 (0x00010000 in little-endian).

---

### Q53. What socket options does the project set?

**A:**

| Option | Value | Purpose |
|--------|-------|---------|
| `SO_REUSEADDR` | 1 | Allow binding to a port in TIME_WAIT state |
| `TCP_NODELAY` | 1 | Disable Nagle's algorithm (reduce latency) |
| `SO_RCVBUF` | 1 MB | Larger receive buffer for burst absorption |

`TCP_NODELAY` is critical for the ingest path — Nagle's algorithm would buffer small frames (7 + topic + payload bytes) and introduce up to 200 ms latency.

---

## G — Metrics & Control

### Q54. How does MetricRegistry work?

**A:** Meyers' singleton holding a map of named atomic counters:

```cpp
class MetricRegistry {
    std::unordered_map<std::string, std::atomic<uint64_t>> counters_;
    std::shared_mutex mutex_;

    void increment(const std::string& name) {
        auto it = counters_.find(name);
        it->second.fetch_add(1, std::memory_order_relaxed);
    }

    MetricSnapshot snapshot() {
        std::shared_lock lock(mutex_);
        // Copy all counters to a snapshot struct
    }
};
```

**Relaxed ordering** on `fetch_add` because we only need eventual accuracy, not inter-counter ordering.

---

### Q55. What is the LatencyHistogram?

**A:** A fixed-bucket histogram using log₂ bucketing:

```
Bucket 0: [0, 1) μs
Bucket 1: [1, 2) μs
Bucket 2: [2, 4) μs
Bucket 3: [4, 8) μs
...
Bucket 20: [524288, 1048576) μs
```

Each bucket is an `std::atomic<uint64_t>`. Recording a latency:

```cpp
void record(uint64_t microseconds) {
    int bucket = (microseconds == 0) ? 0 : (1 + log2(microseconds));
    buckets_[bucket].fetch_add(1, relaxed);
}
```

**Why log₂?** Latency distributions are long-tailed. Linear buckets would waste space on the common case (fast events) and lose resolution on the tail (slow events). Log₂ gives equal resolution at every order of magnitude.

---

### Q56. How does ControlPlane decide pressure level?

**A:** A threshold-based state machine:

```cpp
if (fill_ratio >= 0.95) return EMERGENCY;
if (fill_ratio >= 0.90) return CRITICAL;
if (fill_ratio >= 0.75) return DEGRADED;
if (fill_ratio >= 0.60) return ELEVATED;
return HEALTHY;
```

**Hysteresis** prevents oscillation: to step *down*, the ratio must drop below a lower threshold than the one that triggered the step up. E.g., CRITICAL triggers at 90% but clears at 85%.

---

### Q57. What is `PipelineStateManager`?

**A:** An atomic state variable shared between ControlPlane and Dispatcher:

```cpp
class PipelineStateManager {
    std::atomic<PipelineState> state_{PipelineState::RUNNING};

    void setState(PipelineState s) {
        state_.store(s, std::memory_order_release);
    }
    PipelineState getState() const {
        return state_.load(std::memory_order_acquire);
    }
};
```

States: `RUNNING`, `PAUSED`, `DRAINING`, `STOPPED`.

The Dispatcher checks `getState()` every loop iteration. If `PAUSED`, it sleeps for 100 ms instead of popping from the MPSC queue. This throttles ingestion without losing data.

---

### Q58. What does AdminLoop do?

**A:** A background thread that runs every 10 seconds:

```cpp
while (running_) {
    auto snapshot = MetricRegistry::getInstance().snapshot();
    spdlog::info("Events: {} processed, {} dropped, {} DLQ",
        snapshot.processed, snapshot.dropped, snapshot.dlq_size);
    spdlog::info("Pressure: {}", to_string(controlPlane_.currentLevel()));
    std::this_thread::sleep_for(10s);
}
```

It's a **monitoring loop** — it doesn't modify state, only reads. It uses the thread_local caching in MetricRegistry so it doesn't contend with the hot path.

---

## H — Patterns & Architecture

### Q59. How would you add a new processor type?

**A:**

1. Create `NewProcessor` inheriting from `EventProcessor`.
2. Implement `process()`, `start()`, `stop()`.
3. Add a new `EventPriority` level or reuse an existing one.
4. Register in `EventBus` with a new queue.
5. Add to `ProcessManager` so it gets its own thread.
6. Update `TopicTable` schema to allow topics to target the new processor.

The architecture is open for extension — the `EventProcessor` interface is the extension point.

---

### Q60. What is the Composite pattern in this project?

**A:** `CompositeAlertHandler` holds a vector of `AlertHandler` pointers:

```cpp
class CompositeAlertHandler : public AlertHandler {
    std::vector<AlertHandlerPtr> handlers_;

    void onAlert(const Alert& alert) override {
        for (auto& h : handlers_)
            h->onAlert(alert);
    }
};
```

The caller treats a single handler and a group of handlers uniformly — both implement `onAlert()`. This lets you combine logging, callback, and email handlers without changing the processor code.

---

### Q61. Why is `NullAlertHandler` useful?

**A:** It implements the AlertHandler interface but does nothing:

```cpp
class NullAlertHandler : public AlertHandler {
    void onAlert(const Alert&) override { /* no-op */ }
};
```

Used in tests to suppress alert side effects. Also a reasonable default when no alert handler is configured — avoids null checks everywhere.

---

### Q62. How does the C API bridge enable polyglot SDKs?

**A:** The bridge (`esccore.h`) exposes a flat C API:

```c
esc_engine_t*  esc_engine_create(const char* config_path);
int            esc_engine_submit(esc_engine_t*, const esc_event_t*);
void           esc_engine_destroy(esc_engine_t*);
```

**Why C?** Every language has a C FFI:
- Python: `ctypes.cdll.LoadLibrary("libesccore.so")`
- Go: `// #cgo LDFLAGS: -lesccore`
- Rust: `extern "C" { fn esc_engine_create(...); }`
- Java: JNI or Panama
- Node.js: `ffi-napi`

The C API is an **exception firewall** — all C++ exceptions are caught and converted to error codes before crossing the boundary. This prevents undefined behavior (C has no exceptions).

---

### Q63. What is the difference between the event pool and the DLQ?

**A:**

| Aspect | Event Pool | Dead-Letter Queue |
|--------|-----------|-------------------|
| Purpose | Reuse event memory | Store failed events |
| When used | Every event | Only on failure |
| Ownership | Pool owns, lends via shared_ptr | DLQ owns permanently |
| Lifetime | Returned to pool on refcount=0 | Stays until drained externally |
| Bounded | Yes (1024 slots) | Yes (1000 entries) |

They serve opposite purposes: the pool optimises the happy path (reuse memory), the DLQ handles the sad path (preserve evidence of failures).

---

### Q64. How would you add persistence/replay to the DLQ?

**A:**

1. Add a `StorageEngine::storeDLQ(DLQEntry)` that writes to a separate file.
2. Add a `replayDLQ()` that reads entries and re-submits them to the MPSC queue.
3. Track retry count in the DLQ entry to prevent infinite loops.
4. Add a configurable max-retry limit.

The existing DLQ is in-memory only — a process crash loses all DLQ entries. Persistence would fix that.

---

### Q65. Summarise the top 5 things that make this codebase "interview-ready."

**A:**

1. **Lock-free data structures** (SPSC, MPSC, CAS dedup) — demonstrates deep understanding of atomics, memory ordering, and hardware.
2. **NUMA-aware design** — shows awareness of hardware topology and its performance impact.
3. **Multi-tier processing** (Realtime/Transactional/Batch) — demonstrates system design thinking about latency vs throughput tradeoffs.
4. **5-level backpressure with hysteresis** — shows control theory concepts applied to software.
5. **C API bridge + polyglot SDKs** — demonstrates FFI design, cross-language interop, and API design principles.

---

## J — OS Internals & System Calls

### Q66. What is a `futex` and how does `std::mutex` use it?

**A:** A futex (fast userspace mutex) is a Linux kernel primitive. The key insight: **uncontended lock/unlock never enters the kernel.**

```
Lock (uncontended):
  atomic CAS(futex_word, 0 → 1)  ← pure userspace, ~25ns
  
Lock (contended):
  atomic CAS fails (already 1)
  syscall: futex(FUTEX_WAIT, &futex_word, 1)  ← kernel puts thread to sleep
  
Unlock:
  atomic store(futex_word, 0)
  syscall: futex(FUTEX_WAKE, &futex_word, 1)  ← kernel wakes one waiter
```

`std::mutex` (via pthreads) uses futex on Linux. The project's mutexes (TopicTable, EventBus deques, StorageEngine) all benefit from this optimization — in the common case, there's no syscall.

---

### Q67. Explain `epoll` internals — how does it achieve O(1) event notification?

**A:** `epoll` uses a **red-black tree** for the interest set and a **linked list** for the ready set.

```
epoll_ctl(ADD, fd):
  Insert fd into red-black tree → O(log N)
  Register callback with kernel: when fd becomes readable, add to ready list
  
Data arrives on fd:
  NIC interrupt → kernel network stack → socket receive buffer fills
  Kernel callback fires → appends fd to epoll's ready list → O(1)
  
epoll_wait():
  If ready list non-empty → copy to userspace → O(ready)
  If empty → sleep until callback fires
```

Unlike `select`/`poll` (which scan all N fds on every call), `epoll_wait` returns only the ready fds. Cost is O(ready), not O(total).

---

### Q68. What is `mmap` and how could it be used in StorageEngine?

**A:** `mmap` maps a file into the process's virtual address space. Reads/writes become memory accesses — the kernel handles page faults and I/O transparently.

```cpp
int fd = open("events.dat", O_RDWR);
void* addr = mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
// Now addr[0..file_size-1] IS the file

Event* events = static_cast<Event*>(addr);
events[0].priority = HIGH;  // This writes directly to the file (eventually)
```

**Advantages for StorageEngine:**
- No `read()`/`write()` syscalls — 100ns per access vs. ~1μs per syscall.
- Kernel manages page cache automatically — no manual buffering.
- `msync()` for durability guarantees.

**Disadvantage:** Random access patterns cause excessive page faults. Sequential writes (like StorageEngine's append pattern) are fine.

---

### Q69. What is a page fault and when do they occur in the project?

**A:** A page fault occurs when accessing a virtual address that isn't currently mapped to a physical page.

**Minor page fault (soft):** Page exists in RAM but isn't in the process's page table. Cost: ~1μs. Happens on first access to `mmap`'d memory or newly allocated memory.

**Major page fault (hard):** Page must be loaded from disk. Cost: ~1-10ms (SSD) or ~10-100ms (HDD).

**In the project:**
- `new Event()`: first access to allocated memory → minor page fault.
- Event pool pre-allocates → page faults happen during initialization, not on the hot path.
- StorageEngine file writes → if the file is in page cache, no fault; if not, major fault.

**`mlock()` prevents page faults** by pinning pages in RAM. The NUMA binding code could use this for the SPSC/MPSC queues to guarantee zero page faults on the hot path.

---

### Q70. Explain `SO_REUSEPORT` — how does it enable multi-process scaling?

**A:** With `SO_REUSEPORT`, multiple processes can bind to the same TCP port. The kernel load-balances incoming connections across all bound sockets using a hash of the source IP/port.

```
Process 1: bind(port 8080), listen(), accept() → gets ~33% of connections
Process 2: bind(port 8080), listen(), accept() → gets ~33% of connections
Process 3: bind(port 8080), listen(), accept() → gets ~33% of connections
```

This enables **horizontal scaling without a load balancer**: fork 3 instances of EventStreamCore, each binds to port 8080. The kernel distributes connections. Each process has its own MPSC queue, pipeline, and processors.

---

## K — Compiler, Linker & Build System

### Q71. What is the One Definition Rule (ODR) and how does the project handle it?

**A:** ODR: Every function, variable, class, and template must have exactly one definition across all translation units. Violations cause UB (typically linker errors or silent wrong behavior).

**In the project:**
- Header-only classes (EventPool, queues) are defined in headers → included in multiple `.cpp` files. This is safe because templates and inline functions are ODR-exempt (as long as all definitions are identical).
- `spdlog` is header-only → same exemption.
- `EventFactory::global_event_id` is a `static` class member defined in `event_factory.cpp` → one definition.

**Danger zone:** If two `.cpp` files include different versions of a header (e.g., one has `#define CAPACITY 1024`, another has `#define CAPACITY 2048`), the template instantiations differ → ODR violation → UB.

---

### Q72. What is Link-Time Optimization (LTO) and how would it help this project?

**A:** LTO delays optimization until link time, when the compiler sees all translation units together.

```cmake
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)  # Enable LTO
```

**Benefits for EventStreamCore:**
- **Cross-file inlining:** `EventFactory::createEvent()` can be inlined into `IngestServer::handleClient()` even though they're in different `.cpp` files.
- **Dead code elimination:** Unused functions across `.o` files are removed.
- **Devirtualization:** If the compiler proves that `processor->process()` always calls `RealtimeProcessor::process()`, it removes the virtual dispatch.

**Cost:** Longer link times (minutes instead of seconds for large projects). The project is small enough that LTO link time is ~5 seconds.

---

### Q73. What is symbol visibility and how does `-fvisibility=hidden` help?

**A:** On Linux, shared libraries export all symbols by default. With `-fvisibility=hidden`, all symbols are hidden unless explicitly marked `__attribute__((visibility("default")))`.

**Benefits:**
- **Faster loading:** `dlopen` processes fewer symbols.
- **Smaller binary:** `.dynsym` table shrinks.
- **No symbol conflicts:** Two `.so` files can have internal functions with the same name without collision.
- **Better optimization:** The compiler knows hidden functions can't be called externally → can inline/optimize more aggressively.

The project's C bridge uses `extern "C"` with default visibility for the public API; all C++ internals are hidden.

---

### Q74. What happens during `#include` — the preprocessor in detail?

**A:** The preprocessor is a text substitution engine that runs before compilation:

1. `#include "file.hpp"` → literally copy-paste the file's contents at this point.
2. `#define X 42` → replace all occurrences of `X` with `42`.
3. `#ifdef / #ifndef / #endif` → conditionally include/exclude text.
4. `#pragma once` → skip this file if already included.

**Include guard vs. `#pragma once`:**
```cpp
// Include guard (portable, standard)
#ifndef EVENT_BUS_HPP
#define EVENT_BUS_HPP
// ... contents ...
#endif

// #pragma once (non-standard but supported everywhere, faster)
#pragma once
```

The project uses `#pragma once` throughout. It's faster because the preprocessor can skip re-opening the file entirely, while include guards require opening the file and seeing the `#ifndef` fail.

---

## L — Advanced Concurrency Scenarios

### Q75. What is priority inversion and does it occur in this project?

**A:** Priority inversion: a high-priority thread waits for a lock held by a low-priority thread, while a medium-priority thread preempts the low-priority thread — the high-priority thread is effectively blocked by the medium-priority thread.

**In the project:** The RealtimeProcessor (high priority) might call `StorageEngine::store()` which locks `storageMutex`. If the BatchProcessor (low priority) holds this lock during a large flush, and the TransactionalProcessor (medium priority) is running on the same core — classic priority inversion.

**Mitigation:** Use `pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)`. With priority inheritance, when the RealtimeProcessor blocks on `storageMutex`, the kernel temporarily boosts the BatchProcessor's priority to match — preventing the TransactionalProcessor from preempting it.

---

### Q76. What is a livelock? Give an example related to the project.

**A:** A livelock is when threads keep running but make no progress — they keep responding to each other's state changes without advancing.

**Hypothetical example:** If the backpressure system worked like this:
1. Queue fills → set PAUSED → Dispatcher stops.
2. Queue drains slightly → set RUNNING → Dispatcher resumes.
3. Dispatcher pushes one event → queue fills again → set PAUSED.
4. Repeat forever — the system is "alive" but processing one event per cycle.

**Prevented by:** The project's hysteresis (DEGRADED doesn't return to HEALTHY until metrics improve significantly) and the 10-second evaluation interval (changes happen at most every 10 seconds, giving the system time to stabilize).

---

### Q77. What is the ABA problem in lock-free programming?

**A:** Thread 1 reads value A from a CAS target. Thread 2 changes it to B, then back to A. Thread 1's CAS succeeds (value is still A) but the state has changed.

**In the project's dedup map:**
```cpp
// Bucket contains event_id=42, timestamp=T1
// Thread 1: read (42, T1), prepare CAS to insert new event
// Thread 2: entry expires → CAS (42,T1) → (0,0) → new event CAS (0,0) → (99,T2)
// Thread 2: (99,T2) expires → CAS to (0,0)
// Thread 1: CAS (0,0) == (0,0)? Hmm, but the entry was NOT what Thread 1 originally read

// Actually safe here because:
// 1. Dedup entries are (event_id, timestamp) pairs — not reusable pointers
// 2. The CAS checks both event_id AND timestamp atomically (128-bit CAS on x86)
// 3. Same event_id + different timestamp = different entry → CAS fails correctly
```

**General fix:** Use tagged pointers (include a counter with every CAS value). The project's combined (id, timestamp) effectively serves as a tag.

---

### Q78. How would you detect a deadlock in the project?

**A:** A deadlock requires: mutual exclusion, hold-and-wait, no preemption, circular wait.

**Detection methods:**
1. `gdb`: attach to the hung process, `info threads`, `thread apply all bt` — shows where each thread is blocked.
2. `valgrind --tool=helgrind`: detects lock ordering violations at runtime.
3. `ThreadSanitizer (TSan)`: compile with `-fsanitize=thread`, reports lock-order inversions.

**The project prevents deadlocks by:**
- Most hot paths use lock-free structures (no locks to deadlock on).
- Locks are acquired in consistent order: registry mutex → storage mutex → bus queue mutex.
- No lock is held while calling user code (observer callbacks receive a copy, not a locked reference).

---

### Q79. What is false sharing and how does `alignas(64)` prevent it?

**A:** False sharing: two threads access different variables that happen to be on the same cache line (64 bytes). Each write invalidates the other thread's cache line copy, causing unnecessary cache coherence traffic.

```cpp
struct BadLayout {
    std::atomic<uint64_t> head;  // 8 bytes
    std::atomic<uint64_t> tail;  // 8 bytes — same cache line as head!
};
// Producer writes head, consumer reads tail → false sharing
```

```cpp
struct GoodLayout {
    alignas(64) std::atomic<uint64_t> head;  // Own cache line
    alignas(64) std::atomic<uint64_t> tail;  // Own cache line
};
```

**In the project:** SPSC and MPSC queues use `alignas(64)` on `head_` and `tail_` to eliminate false sharing. This can improve throughput by 2-10× on multi-core systems.

---

### Q80. What is `std::memory_order` and when is each ordering needed?

**A:**

| Ordering | Guarantee | Cost (x86) | Used In Project |
|----------|-----------|------------|----------------|
| `relaxed` | No ordering, just atomicity | Free (MOV) | Metric counters, running_ flags |
| `acquire` | This load sees all writes before the matching release | Free (MOV, x86 has strong model) | SPSC consumer reading head |
| `release` | All writes before this are visible to the acquire | Free (MOV, x86 has strong model) | SPSC producer writing tail |
| `acq_rel` | Both acquire and release | Free on x86 | MPSC CAS operations |
| `seq_cst` | Total global order across all threads | MFENCE (~20 cycles) | Default, used when unsure |

**x86 is "almost free":** The x86 memory model is Total Store Order (TSO) — stores are ordered with respect to each other, loads are ordered with respect to each other. Only store→load reordering is allowed. So `acquire` and `release` map to plain MOV instructions. Only `seq_cst` stores need an MFENCE.

**ARM64 is weaker:** ARM allows both store→load and load→store reordering. `acquire` maps to LDAR, `release` maps to STLR. These are more expensive than plain loads/stores.

---

## M — Error Handling & Exception Safety

### Q81. What are the three exception safety guarantees?

**A:**

| Guarantee | Definition | Example in Project |
|-----------|------------|-------------------|
| **No-throw** | Operation never throws. Marked `noexcept`. | Destructors (`~Dispatcher() noexcept`), signal handler, atomic operations |
| **Strong** | If an exception is thrown, state is rolled back to before the call. | `EventFactory::createEvent()` — if CRC computation throws (it won't, but hypothetically), no partial event exists |
| **Basic** | If an exception is thrown, the object is in a valid but unspecified state. No leaks. | `EventBus::push()` with `BLOCK_PRODUCER` — if `wait_for` throws, the mutex is released by `unique_lock` destructor |

**In practice:** Most of the project offers the **basic guarantee** via RAII (lock guards, smart pointers). The no-throw guarantee is explicitly applied to destructors and the signal handler.

---

### Q82. Why must destructors be `noexcept`?

**A:** If a destructor throws during stack unwinding (while handling another exception), `std::terminate()` is called — the program crashes.

```cpp
// In the project:
Dispatcher::~Dispatcher() noexcept {
    stop();  // stop() must not throw
}
```

Since C++11, destructors are **implicitly `noexcept`** unless explicitly declared otherwise. The project adds explicit `noexcept` for documentation clarity.

**What if `stop()` could throw?** Wrap in try/catch:
```cpp
~Dispatcher() noexcept {
    try { stop(); }
    catch (...) { /* log and swallow */ }
}
```

---

### Q83. How does the project handle exceptions in processor threads?

**A:** `ProcessManager::runLoop()` has a try/catch at the top level:

```cpp
while (isRunning_) {
    try {
        auto event = bus.pop(queueId, timeout);
        if (event) processor->process(*event);
    } catch (const std::exception& e) {
        spdlog::error("Processor exception: {}", e.what());
        metrics.increment("processor_exceptions");
        dlq_.push(DLQEntry{event, e.what(), now()});
    } catch (...) {
        spdlog::critical("Unknown exception in processor");
    }
}
```

**Three layers of defense:**
1. `std::exception&` catches all standard exceptions → log + DLQ.
2. `catch(...)` catches non-standard throws (e.g., `throw 42;`) → log.
3. The loop continues — one bad event doesn't kill the processor.

**What's NOT caught:** `std::terminate()` (from `noexcept` violation), signals (`SIGSEGV`), `std::abort()`. These are fatal.

---

### Q84. What is the exception firewall pattern in the C bridge?

**A:** Every C API function wraps the entire body in try/catch:

```cpp
extern "C" int esccore_push(EscEvent* raw) {
    try {
        auto event = toInternal(raw);
        g_engine->submit(event);
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Bridge error: {}", e.what());
        return -1;
    } catch (...) {
        return -2;
    }
}
```

**Why?** C has no exception mechanism. If a C++ exception propagates through a C stack frame (e.g., Python calling via ctypes → C API → C++ throws), the behavior is **undefined** — typically a crash with no diagnostics.

The firewall converts every exception to an integer error code. The SDK caller checks the return value.

---

### Q85. What is `std::current_exception` and when would you use it?

**A:** `std::current_exception()` captures the currently active exception as a `std::exception_ptr` — a type-erased, copyable handle.

```cpp
std::exception_ptr captured;
try {
    processor->process(event);
} catch (...) {
    captured = std::current_exception();
}
// Later, in another thread or context:
if (captured) std::rethrow_exception(captured);
```

**Use in the project:** The DLQ could store `std::exception_ptr` alongside the failed event, allowing a replay system to inspect the original exception. Currently, only the `what()` string is stored.

---

### Q86. What happens if `std::bad_alloc` is thrown during event processing?

**A:** `std::bad_alloc` means `new` failed — the system is out of memory.

**In the project:**
1. The processor's try/catch catches it (it inherits from `std::exception`).
2. The event goes to DLQ... but DLQ insertion also allocates memory → might throw again.
3. If the second allocation also fails, the `catch(...)` block catches it and logs.
4. But `spdlog::error` might also allocate → potential infinite failure cascade.

**Production fix:** Pre-allocate the DLQ and error message buffers. Use a fixed-size ring buffer for DLQ instead of `std::deque` (which allocates on push). The event pool already mitigates this for normal events — extend the same pattern to error handling.

---

### Q87. Explain RAII exception safety in `StorageEngine::store()`.

**A:**

```cpp
void StorageEngine::store(const Event& event) {
    std::lock_guard<std::mutex> lock(storageMutex_);  // 1
    file_.write(data, size);                           // 2
    file_.flush();                                     // 3
}
```

If `write()` (step 2) throws:
- `lock_guard` destructor runs → mutex is released ✅
- `file_` remains open (not corrupted) ✅
- No memory leaks ✅

If `flush()` (step 3) throws:
- Data was written but not flushed → partial write on disk.
- This is the **basic guarantee** — the file is in a valid state (data is there) but the caller doesn't know if it was persisted.

**To upgrade to strong guarantee:** Write to a temporary file, then atomically `rename()` to the final path. But this is overkill for an append-only event log.

---

### Q88. What is `std::terminate` and what triggers it?

**A:** `std::terminate()` is called when the C++ runtime determines that exception handling cannot continue. It calls `std::terminate_handler` (default: `std::abort()`).

**Triggers:**
| Situation | Example in Project |
|-----------|-------------------|
| Uncaught exception in `std::thread` | If `runLoop` didn't have try/catch |
| Exception thrown from `noexcept` function | If `~Dispatcher()` threw |
| Exception during stack unwinding | If a destructor throws while another exception is active |
| `std::thread` destructor called on joinable thread | If `stop()` forgot to call `join()` |
| `std::promise` destroyed without setting value | Not used in project |

**The most insidious:** A joinable `std::thread` being destroyed without `join()` or `detach()`. The project prevents this by calling `join()` in every `stop()` method and in destructors.

---

## N — Testing, Debugging & Sanitizers

### Q89. What testing patterns does the project use?

**A:**

```
unittest/
├── test_spsc_queue.cpp      — SPSC push/pop, full/empty, concurrent
├── test_event_bus.cpp        — Routing, overflow policies, backpressure
├── test_topic_table.cpp      — CRUD, concurrent read/write, missing topic
├── test_event_factory.cpp    — CRC integrity, unique IDs, move semantics
├── test_metric_registry.cpp  — Counter increments, snapshot consistency
├── test_dlq.cpp              — Push/pop, capacity limit, drain
├── test_event_pool.cpp       — Acquire/release, exhaustion, custom deleter
```

**Pattern:** Each test file focuses on one class. Tests follow **Arrange-Act-Assert**:
```cpp
TEST(SPSCQueue, PushPopSingle) {
    SPSCQueue<int, 4> q;        // Arrange
    q.push(42);                  // Act
    auto val = q.pop();          // Act
    ASSERT_TRUE(val.has_value());// Assert
    EXPECT_EQ(*val, 42);        // Assert
}
```

---

### Q90. How do you test lock-free code?

**A:** Lock-free correctness depends on timing — a single-threaded test won't find bugs. Strategies:

1. **Stress testing:** Multiple threads hammering the queue for millions of iterations:
```cpp
TEST(MPSCQueue, StressMultiProducer) {
    MPSCQueue<int, 65536> q;
    std::atomic<int> sum{0};
    // 8 producers push 100K each
    // 1 consumer pops all and sums
    // Verify sum matches expected
}
```

2. **ThreadSanitizer (TSan):** Compile with `-fsanitize=thread`. TSan instruments every memory access and detects data races, even those that only manifest under specific timing.

3. **Model checking:** Tools like CDSChecker or Relacy formally verify lock-free algorithms against the C++11 memory model by exploring all possible interleavings.

4. **Targeted delays:** Insert `std::this_thread::yield()` at critical points to widen race windows.

---

### Q91. What is ThreadSanitizer and how does it work?

**A:** TSan is a compile-time instrumentation tool that detects data races.

```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread" ..
make && ./test_spsc_queue
```

**How it works:**
1. Compiler inserts shadow memory reads/writes alongside every memory access.
2. TSan maintains a **vector clock** per thread — a logical timestamp.
3. On each access, TSan checks if another thread accessed the same address with a conflicting operation (read/write or write/write) without a happens-before relationship.
4. If found → reports the race with both stack traces.

**Cost:** 5-15× slowdown, 5-10× memory overhead. Use only in testing, never in production.

**In the project:** All lock-free code (SPSC, MPSC, dedup CAS) should pass TSan clean. TSan understands `std::atomic` orderings and won't false-positive on correctly ordered operations.

---

### Q92. What is AddressSanitizer (ASan)?

**A:** ASan detects memory errors:

| Error | Detection Method |
|-------|-----------------|
| Heap buffer overflow | Red zones around allocations |
| Stack buffer overflow | Red zones around stack variables |
| Use after free | Quarantine zone — freed memory poisoned |
| Use after return | Optional (stack-use-after-return) |
| Double free | Shadow memory check |
| Memory leaks | LeakSanitizer (integrated) |

```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" ..
```

**In the project:** ASan would catch:
- Buffer overrun in `parseFrame()` if `topic_len` is corrupted.
- Use-after-free if a pooled event is accessed after return to pool.
- Memory leak if `IngestEventPool` custom deleter has a bug.

**Cost:** 2× slowdown, 3× memory. Cheaper than TSan, can be used in CI.

---

### Q93. What is UndefinedBehaviorSanitizer (UBSan)?

**A:** Detects undefined behavior at runtime:

```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=undefined" ..
```

**Detects:**
- Signed integer overflow
- Null pointer dereference
- Misaligned pointer access
- Shift by amount ≥ bit width
- Division by zero
- Accessing out-of-bounds array index

**In the project:** UBSan would catch:
- If `calculateCRC32` shifts by ≥ 32 bits (it doesn't, but a typo could).
- If `ntohl` result overflows a signed variable.
- If `HighPerformanceEvent::topic_len` exceeds 128 → `memcpy` overruns `topic[]`.

---

### Q94. How do you debug a hung process with `gdb`?

**A:**

```bash
# Attach to running process
gdb -p $(pidof eventstream_core)

# Show all threads
(gdb) info threads
  Id   Target Id         Frame
  1    Thread 0x7f...    futex_wait (...)           ← main thread, sleeping
  3    Thread 0x7f...    __lll_lock_wait (...)      ← STUCK on mutex!
  5    Thread 0x7f...    epoll_wait (...)           ← waiting for I/O

# Get backtrace of stuck thread
(gdb) thread 3
(gdb) bt
#0  __lll_lock_wait () at ...
#1  pthread_mutex_lock () at ...
#2  StorageEngine::store () at storage_engine.cpp:42
#3  ProcessManager::runLoop () at process_manager.cpp:88

# Check mutex state
(gdb) print storageMutex_.__data.__owner
$1 = 12345    ← thread 12345 holds the lock

# Find that thread
(gdb) info threads | grep 12345
  7    Thread 0x7f...    BatchProcessor::flush() at batch_processor.cpp:120
```

**Diagnosis:** Thread 3 (RealtimeProcessor) is waiting for `storageMutex_` held by thread 7 (BatchProcessor). If thread 7 is also waiting for something → deadlock.

---

### Q95. How do you use `perf` to find hot spots?

**A:**

```bash
# Record CPU samples for 30 seconds
perf record -g ./eventstream_core &
sleep 30 && kill -INT %1

# Show flat profile
perf report --sort=symbol
  29.3%  SPSCQueue::push
  18.7%  Dispatcher::dispatchLoop
  12.1%  EventFactory::calculateCRC32
   8.4%  IngestServer::parseFrame

# Generate flame graph
perf script | stackcollapse-perf.pl | flamegraph.pl > flames.svg
```

**What the output tells you:**
- 29.3% in `SPSCQueue::push` → this is the hottest function. Check if the queue is frequently full (spinning on full check) or if false sharing is occurring.
- 12.1% in CRC-32 → expected for payload integrity checks. Could accelerate with `_mm_crc32_u64` (SSE4.2 hardware CRC).

---

### Q96. What is `strace` and when would you use it on this project?

**A:** `strace` traces system calls made by a process.

```bash
strace -e trace=network -p $(pidof eventstream_core)
# Output:
accept4(3, {sa_family=AF_INET, sin_port=htons(54321)}, ...) = 7
recv(7, "\x00\x00\x00\x1e\x02\x00\x05"..., 4, MSG_WAITALL) = 4
recv(7, "..."..., 26, MSG_WAITALL) = 26
```

**Use cases in the project:**
- **Connection debugging:** See if `accept()` is being called, verify `setsockopt` values.
- **Performance:** Count syscalls per second — too many means the hot path is entering the kernel.
- **Hang diagnosis:** See which syscall a thread is stuck on (`futex`, `recv`, `write`).

```bash
# Count syscalls over 10 seconds
strace -c -p $(pidof eventstream_core)
% time     seconds  usecs/call     calls    errors syscall
 78.23    0.120000          12     10000             futex
 15.42    0.024000           4      6000             recv
  3.11    0.005000          50       100             write
```

If `futex` dominates → lock contention. If `recv` dominates → I/O bound.

---

### Q97. How would you write a mock `StorageEngine` for testing?

**A:**

```cpp
class MockStorageEngine : public StorageEngine {
public:
    std::vector<Event> stored_events;
    int fail_after = -1;  // -1 = never fail
    int call_count = 0;

    void store(const Event& event) override {
        call_count++;
        if (fail_after >= 0 && call_count > fail_after) {
            throw std::runtime_error("Simulated disk failure");
        }
        stored_events.push_back(event);
    }
};

TEST(Processor, EventGoesToDLQOnStorageFailure) {
    MockStorageEngine mock;
    mock.fail_after = 0;  // Fail on first call
    DLQManager dlq(100);
    RealtimeProcessor proc(mock, dlq);
    
    Event e = EventFactory::createEvent(...);
    proc.process(e);
    
    EXPECT_EQ(mock.stored_events.size(), 0);  // Not stored
    EXPECT_EQ(dlq.size(), 1);                  // Went to DLQ
}
```

**Key:** Inject the mock via constructor (dependency injection). The project's `ProcessManager` takes `StorageEngine&` — replace with `MockStorageEngine` in tests.

---

## O — Performance Profiling & Optimization

### Q98. What is branch prediction and how does it affect the project?

**A:** Modern CPUs predict which way a branch (`if/else`) will go and speculatively execute that path. A misprediction costs ~15-20 cycles (pipeline flush).

**In the project:**
```cpp
// Dispatcher hot loop:
if (event->priority == CRITICAL) { ... }    // Rare — ~1% of events
else if (event->priority == HIGH) { ... }   // Common — ~30%
else if (event->priority == MEDIUM) { ... } // Most common — ~60%
else { ... }                                 // LOW — ~9%
```

**Optimization:** Order branches by frequency. Put MEDIUM first, HIGH second, CRITICAL/LOW last. This improves branch prediction hit rate.

**`[[likely]]` / `[[unlikely]]` (C++20):**
```cpp
if (queue.full()) [[unlikely]] {
    // Overflow handling — rarely executed
}
```

The project targets C++17, but GCC/Clang support `__builtin_expect`:
```cpp
if (__builtin_expect(queue.full(), 0)) { ... }
```

---

### Q99. What is cache-oblivious vs cache-aware design?

**A:**

| Approach | Description | Project Usage |
|----------|-------------|---------------|
| **Cache-aware** | Explicitly designed for specific cache line sizes (`alignas(64)`) | SPSC, MPSC, HighPerformanceEvent |
| **Cache-oblivious** | Algorithm works well on any cache hierarchy without knowing sizes | Not used — project is explicitly cache-aware |

**Trade-off:** Cache-aware designs are faster on the target hardware but break if cache line size changes (e.g., 128 bytes on Apple M1). Cache-oblivious designs are portable but can't achieve peak performance.

The project's `alignas(64)` is correct for x86-64. For portability:
```cpp
constexpr size_t CACHE_LINE = std::hardware_destructive_interference_size; // C++17
```

This evaluates to the actual cache line size at compile time (64 on x86, 128 on ARM Apple).

---

### Q100. How would you measure end-to-end latency?

**A:**

```cpp
// In IngestServer::handleClient():
auto t_start = std::chrono::steady_clock::now();
event->metadata["ingest_ns"] = std::to_string(
    t_start.time_since_epoch().count());

// In ProcessManager::runLoop(), after process():
auto t_end = std::chrono::steady_clock::now();
auto t_start_ns = std::stoull(event->metadata["ingest_ns"]);
auto latency = t_end.time_since_epoch().count() - t_start_ns;
latencyHistogram.record(latency / 1000);  // ns → μs
```

**Why `steady_clock`?** It's monotonic — never goes backward (unlike `system_clock` which can be adjusted by NTP). Important for latency measurement.

**Overhead:** `steady_clock::now()` is ~20ns on Linux (uses `CLOCK_MONOTONIC` via `clock_gettime` vDSO).

---

### Q101. What is instruction-level parallelism (ILP) and how does CRC-32 use it?

**A:** Modern CPUs can execute multiple independent instructions simultaneously (superscalar execution).

The table-based CRC-32 has a loop-carried dependency:
```cpp
for (size_t i = 0; i < len; i++)
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
//  ^^^                                     ^^^
//  Each iteration depends on the previous crc value
```

This limits ILP — the CPU can't start iteration `i+1` until iteration `i` computes `crc`. Throughput: ~1 byte per cycle.

**Optimization:** Process 4/8 bytes per iteration using multiple independent CRC accumulators (slice-by-4 / slice-by-8 algorithm). Or use hardware CRC: `_mm_crc32_u64` processes 8 bytes per cycle.

---

### Q102. What is prefetching and how would it help the project?

**A:** CPU prefetch instructions load data into cache before it's needed.

```cpp
// In Dispatcher, before processing event:
auto* next_event = mpsc_queue_.peek_next();
if (next_event) {
    __builtin_prefetch(next_event, 0, 3);  // read, high temporal locality
}
dispatcher_process(current_event);  // By the time this finishes,
                                     // next_event is in L1 cache
```

**For SPSC queue traversal:**
```cpp
T pop() {
    T item = buffer_[tail_ % capacity_];
    // Prefetch the NEXT item while the CPU processes this one
    __builtin_prefetch(&buffer_[(tail_ + 1) % capacity_], 0, 1);
    tail_.store(tail_ + 1, release);
    return item;
}
```

**Measured impact:** Prefetching typically improves throughput by 10-30% for pointer-chasing workloads (MPSC linked list). Less impact on contiguous arrays (SPSC buffer) because the hardware prefetcher already handles sequential access.

---

### Q103. What is the impact of `std::shared_ptr` on performance?

**A:**

Every copy of `shared_ptr` increments the reference count (atomic `fetch_add`). Every destruction decrements it (atomic `fetch_sub`). The final destruction also calls the deleter.

```
Event lifecycle in the pipeline:
  1. IngestPool::acquire() → refcount = 1
  2. MPSC push (move) → refcount = 1 (no increment)
  3. Dispatcher copies to EventBus → refcount = 2
  4. Observer notification (copy) → refcount = 3
  5. Observer done → refcount = 2
  6. Processor done → refcount = 1
  7. EventBus done → refcount = 0 → custom deleter → return to pool
```

**Atomic operations per event:** ~6 (increments + decrements).
**Cost per atomic operation:** ~5ns on uncontended cache line, ~40ns if cache line is shared across cores.
**Total overhead:** 30-240ns per event from reference counting alone.

**Optimization:** Move instead of copy wherever possible. The project moves events through the MPSC queue (no refcount bump). The EventBus push could be optimized to move instead of copy.

---

### Q104. How does `std::string` small-buffer optimization (SBO) affect the project?

**A:** Most `std::string` implementations store short strings (≤15 or ≤22 bytes) inline — no heap allocation.

**In the project:**
- Topic names (e.g., `"orders"`, `"telemetry"`) are typically < 15 chars → SBO, no allocation.
- Metadata keys (e.g., `"source"`, `"region"`) → SBO.
- Metadata values might be longer → may allocate.
- `HighPerformanceEvent` avoids strings entirely — uses `char topic[128]` for guaranteed no-allocation.

**Impact:** For the common case (short topic names), `std::string` in `Event` is effectively as fast as a fixed-size char array. For long topics (> 22 chars), each event construction allocates on the heap — the event pool mitigates this by resetting and reusing the string's buffer.

---

### Q105. What compiler optimizations matter most for this project?

**A:**

| Optimization | Flag | Impact |
|-------------|------|--------|
| **Inlining** | `-O2` / `-finline-functions` | Eliminates function call overhead for small functions (queue push/pop, metric increment) |
| **Loop unrolling** | `-funroll-loops` | CRC-32 inner loop processes 4 bytes per iteration |
| **Auto-vectorization** | `-O3` / `-ftree-vectorize` | Batch processor's flush loop could be SIMD-ized |
| **Link-Time Optimization** | `-flto` | Cross-file inlining (EventFactory into IngestServer) |
| **Profile-Guided Optimization** | `-fprofile-generate` → `-fprofile-use` | Compiler uses real branch frequencies from profiling run |
| **`-march=native`** | | Enables AVX2, SSE4.2 (hardware CRC), BMI2 (bit manipulation) |

**Most impactful:** `-O2 -march=native -flto`. The `-O2` gives 3-5× speedup over `-O0`. `-march=native` enables hardware CRC. `-flto` enables cross-file inlining.

---

### Q106. Explain the "benchmark" directory — how are benchmarks structured?

**A:** Benchmarks measure throughput and latency of individual components:

```cpp
// SPSC throughput benchmark
void BM_SPSCThroughput(benchmark::State& state) {
    SPSCQueue<Event, 16384> queue;
    for (auto _ : state) {
        for (int i = 0; i < 1000; i++) {
            queue.push(Event{});
            auto e = queue.pop();
            benchmark::DoNotOptimize(e);
        }
    }
    state.SetItemsProcessed(state.iterations() * 1000);
}
```

**`benchmark::DoNotOptimize`:** Prevents the compiler from optimizing away the pop (since the result isn't "used"). Without it, the compiler might remove the entire loop.

**Metrics reported:** Items/second, bytes/second, CPU time per operation.

---

## P — CMake & Build System

### Q107. Explain the CMake dependency resolution strategy.

**A:** The project uses a **local-first fallback** approach:

```cmake
# Try system-installed first
find_package(spdlog QUIET)
if (NOT spdlog_FOUND)
    # Fall back to local source tree
    if (EXISTS "${CMAKE_SOURCE_DIR}/../spdlog/CMakeLists.txt")
        add_subdirectory(${CMAKE_SOURCE_DIR}/../spdlog ${CMAKE_BINARY_DIR}/spdlog)
    else()
        message(FATAL_ERROR "spdlog not found")
    endif()
endif()
```

**Why not FetchContent?** FetchContent downloads from the internet at configure time — not suitable for air-gapped build environments (common in automotive/embedded). Local source trees are version-controlled and reproducible.

---

### Q108. What is the difference between `STATIC` and `SHARED` libraries?

**A:**

| Aspect | Static (`.a`) | Shared (`.so`) |
|--------|--------------|----------------|
| Linking | Copied into executable at link time | Loaded at runtime by `ld.so` |
| Binary size | Larger (includes library code) | Smaller (references library) |
| Updates | Must relink entire executable | Just replace `.so` file |
| Symbol visibility | All symbols available to linker | Only exported symbols |
| Load time | No runtime overhead | `dlopen` + symbol resolution |
| Memory sharing | Each process has its own copy | Shared across processes (code pages) |

**The project uses SHARED** for `libesccore.so` (the C bridge) — it must be loadable by Python (`ctypes`) and Go (`cgo`) at runtime. Static would require linking at compile time, defeating the polyglot purpose.

**The core `eventstream_core` is a STATIC library** — linked into the main executable and the bridge `.so`.

---

### Q109. What is RPATH and why does it matter?

**A:** RPATH is an embedded search path in an ELF binary that tells the dynamic linker where to find `.so` files.

```bash
# Check RPATH
readelf -d eventstream_core | grep RPATH
# (RPATH)  Library rpath: [$ORIGIN/../lib]
```

**`$ORIGIN`** expands to the directory containing the executable. So `$ORIGIN/../lib` means "look in the `lib/` folder next to the executable."

**Without RPATH:** Users must set `LD_LIBRARY_PATH` manually — fragile and error-prone.

**CMake setting:**
```cmake
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
```

---

### Q110. How does CMake handle header-only libraries?

**A:** With `INTERFACE` libraries:

```cmake
add_library(eventstream_headers INTERFACE)
target_include_directories(eventstream_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/include)
```

**INTERFACE** means: no compilation (no `.o` file), only propagates include paths and compile flags to consumers. spdlog is consumed this way.

---

### Q111. What is `CMAKE_EXPORT_COMPILE_COMMANDS`?

**A:** Generates `compile_commands.json` — a database of all compilation commands for every source file.

```cmake
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

**Used by:**
- **clangd** (VS Code C++ IntelliSense) — needs to know include paths and flags.
- **clang-tidy** — static analyzer reads compile commands.
- **Coverity** — uses compile commands for accurate analysis.

The project's Coverity integration (`staticlocaltool/build_coverity.sh`) relies on this file.

---

### Q112. How would you add cross-compilation support?

**A:** Create a CMake toolchain file:

```cmake
# toolchain-aarch64.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
```

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=toolchain-aarch64.cmake ..
```

**Relevance:** The project's `ccu2_Log_branch/` directory contains NXP S32G (ARM Cortex-A53) builds — cross-compilation is already used in the broader workspace. The EventStreamCore engine could be cross-compiled for embedded targets.

---

## Q — Security & Robustness

### Q113. What input validation does the project perform?

**A:**

```
parseFrame() validation:
  1. Length prefix: ntohl(len) must be ≤ MAX_FRAME_SIZE (prevent allocation bomb)
  2. Priority byte: must be 0-3 (valid EventPriority range)
  3. Topic length: must be ≤ remaining bytes (prevent buffer overread)
  4. Topic bytes: must be valid UTF-8 (or at least not contain null bytes)
```

**Missing validation (potential improvements):**
- No rate limiting per connection → one client can flood the pipeline.
- No authentication → any client can connect and push events.
- No TLS → data in transit is unencrypted.
- No maximum connections limit → resource exhaustion attack (spawn 10K connections).

---

### Q114. What is a buffer overflow and where could it occur?

**A:** Writing beyond the allocated bounds of a buffer.

**Potential locations in the project:**

1. **`parseFrame()`:** If `topic_len` in the wire frame is corrupted (says 1000 but payload is only 100 bytes), `memcpy(topic_buf, buf + 7, topic_len)` overreads.

   **Mitigation:** Check `topic_len ≤ total_len - 7` before `memcpy`.

2. **`HighPerformanceEvent`:** `topic[128]` and `payload[400]` are fixed-size. If `topic_len > 128` or `payload_len > 400`, memcpy overflows.

   **Mitigation:** `std::min(topic_len, 128u)` before copy.

3. **CRC table:** `table[256]` indexed by `(crc ^ data[i]) & 0xFF`. The `& 0xFF` mask guarantees index ≤ 255. ✅ Safe.

---

### Q115. What is an integer overflow attack?

**A:** If the 4-byte length prefix is `0xFFFFFFFF` (4 GB), and the code does:
```cpp
uint32_t len = ntohl(len_buf);
char* buf = new char[len];  // Allocates 4 GB!
recv(fd, buf, len, MSG_WAITALL);  // Blocks forever reading 4 GB
```

**Impact:** Out-of-memory (one malicious client kills the server).

**Mitigation:** 
```cpp
if (len > MAX_FRAME_SIZE) {
    close(fd);
    return;  // Reject oversized frames
}
```

`MAX_FRAME_SIZE` should be set to a reasonable value (e.g., 1 MB).

---

### Q116. How would you add TLS to the TCP server?

**A:** Use OpenSSL:

```cpp
SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM);
SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM);

// In handleClient():
SSL* ssl = SSL_new(ctx);
SSL_set_fd(ssl, client_fd);
SSL_accept(ssl);  // TLS handshake

// Replace recv() with:
int n = SSL_read(ssl, buf, len);

// Replace close() with:
SSL_shutdown(ssl);
SSL_free(ssl);
close(fd);
```

**Performance impact:** TLS handshake costs ~1ms (RSA-2048). Data encryption costs ~0.5 μs per 1 KB (AES-256-GCM with AES-NI hardware acceleration). For the project's small events, the handshake dominates — use connection pooling to amortize it.

---

### Q117. What is a denial-of-service (DoS) vulnerability in this project?

**A:** Several potential vectors:

| Attack | Mechanism | Impact | Mitigation |
|--------|-----------|--------|------------|
| Connection flood | Open 10K TCP connections, do nothing | Thread exhaustion (10K threads × 8MB stack) | Connection limit per IP, `epoll` |
| Slow-loris | Send 1 byte at a time, keeping connection alive | Threads blocked on `recv()` forever | Per-connection timeout (5 second idle) |
| Large frame | Send 4GB length prefix | OOM on `new char[4GB]` | MAX_FRAME_SIZE check |
| Topic explosion | Submit events with millions of unique topics | TopicTable unbounded growth, hash collision chains | Maximum topic count, topic whitelist |
| CRC computation | Large payloads with valid length | CPU-bound on CRC-32 | MAX_PAYLOAD_SIZE |

---

### Q118. How would you add authentication?

**A:** Token-based authentication in the wire protocol:

```
Extended frame format:
[4B length][32B auth_token][1B priority][2B topic_len][topic][payload]
```

1. Client includes a pre-shared token (or JWT) in every frame.
2. `parseFrame()` extracts and validates the token.
3. Invalid tokens → close connection immediately.
4. Token maps to a client identity → enables per-client rate limiting and access control.

**Alternative:** Authenticate once at connection establishment (TLS client certificates), then trust all subsequent frames from that connection.

---

## R — STL Internals & Smart Pointers

### Q119. What is the internal structure of `std::shared_ptr`?

**A:**

```
shared_ptr<Event>:
  [pointer to Event]        — 8 bytes
  [pointer to control block] — 8 bytes
  Total: 16 bytes

Control block (heap-allocated):
  [strong reference count]   — atomic<size_t>
  [weak reference count]     — atomic<size_t>
  [deleter (type-erased)]    — function pointer or callable
  [allocator]                — for deallocation
```

**When using `std::make_shared<Event>(...)`:** The control block and Event are allocated in a **single allocation** (combined block). This is faster (one `new` instead of two) and more cache-friendly (data and control block are adjacent).

**When using `std::shared_ptr<Event>(new Event(...))` or custom deleters:** Two separate allocations — one for Event, one for control block.

The project's `IngestEventPool` uses a custom deleter → two allocations per event. The `EventPool` template uses `make_shared` → single allocation.

---

### Q120. What is `std::weak_ptr` and when would you use it in this project?

**A:** `weak_ptr` is a non-owning observer of a `shared_ptr`. It doesn't increment the strong reference count. To access the object, call `lock()` which returns a `shared_ptr` (or empty if the object is deleted).

**Potential use in the project:**
- **Observer pattern:** Instead of `ProcessedEventStream` holding `shared_ptr<Observer>`, hold `weak_ptr<Observer>`. If the observer is destroyed, the weak_ptr becomes empty and is automatically cleaned up during notification. Prevents keeping observers alive longer than their owners intend.

```cpp
void notifyProcessed(const Event& event) {
    auto observers_copy = observers_;  // snapshot
    for (auto it = observers_copy.begin(); it != observers_copy.end(); ) {
        if (auto obs = it->lock()) {
            obs->onEventProcessed(event, name);
            ++it;
        } else {
            it = observers_copy.erase(it);  // Observer is dead
        }
    }
}
```

---

### Q121. What is `enable_shared_from_this`?

**A:** A CRTP base class that allows an object managed by `shared_ptr` to create additional `shared_ptr` instances to itself.

```cpp
class IngestServer : public std::enable_shared_from_this<IngestServer> {
    void start() {
        auto self = shared_from_this();  // Safe shared_ptr to this
        std::thread([self] {
            self->acceptLoop();
        }).detach();
    }
};
```

**Without `enable_shared_from_this`:** Creating `shared_ptr<IngestServer>(this)` would create a second control block → double-free when both `shared_ptr` groups destroy the object.

**Not currently used in the project** because threads are managed via `std::thread` member variables (joined in `stop()`). Would be useful if switching to `std::async` or detached threads.

---

### Q122. What is iterator invalidation and where does it matter?

**A:** Operations on containers can invalidate existing iterators, pointers, and references.

| Container | Operation | Invalidation |
|-----------|-----------|-------------|
| `std::vector` | `push_back` (reallocation) | All iterators, pointers, references |
| `std::vector` | `push_back` (no realloc) | Only `end()` iterator |
| `std::deque` | `push_back` / `push_front` | All iterators (but not references!) |
| `std::unordered_map` | `insert` (rehash) | All iterators |
| `std::unordered_map` | `insert` (no rehash) | None |
| `std::list` | any insert/erase | Only erased element's iterator |

**In the project:**
- **EventBus deque queues:** `push_back` invalidates iterators. The code uses index-based access or `front()`/`back()` — safe.
- **TopicTable unordered_map:** `registerTopic()` could trigger rehash → invalidates iterators. Safe because all access is under `unique_lock`.
- **`ProcessedEventStream::observers_` vector:** `subscribe()` does `push_back` → could invalidate iterators used by `notifyProcessed()`. Safe because `notifyProcessed()` copies the vector first.

---

### Q123. What is the allocator model in STL and how does it relate to NUMA?

**A:** Every STL container takes an optional allocator template parameter:

```cpp
template<typename T, typename Alloc = std::allocator<T>>
class vector;
```

For NUMA-aware containers:
```cpp
template<typename T>
class NumaAllocator {
    int node_;
public:
    NumaAllocator(int node) : node_(node) {}
    T* allocate(size_t n) {
        return static_cast<T*>(numa_alloc_onnode(n * sizeof(T), node_));
    }
    void deallocate(T* p, size_t n) {
        numa_free(p, n * sizeof(T));
    }
};

// MPSC queue with NUMA-local storage:
std::deque<Event, NumaAllocator<Event>> queue(NumaAllocator<Event>(0));
```

**The project doesn't use custom allocators** — it uses `numa_alloc_onnode` directly for the ring buffer. Custom allocators would allow NUMA-aware STL containers throughout.

---

### Q124. What is `std::pmr` (polymorphic memory resource)?

**A:** C++17 added `std::pmr::` containers that use runtime-polymorphic allocators:

```cpp
#include <memory_resource>

char buffer[1024 * 1024];  // 1 MB stack buffer
std::pmr::monotonic_buffer_resource pool(buffer, sizeof(buffer));
std::pmr::vector<Event> events(&pool);

// All allocations come from the stack buffer — zero heap allocations
events.push_back(Event{});
```

**Relevance to project:** The batch processor accumulates events for 5 seconds, then flushes. Instead of heap-allocating each event in the batch:
1. Allocate a large buffer at start.
2. Use `monotonic_buffer_resource` for the batch window.
3. After flush, reset the resource (instant "deallocation" — just reset the pointer).

This is faster than `new`/`delete` per event and avoids memory fragmentation.

---

### Q125. Explain `std::move` vs `std::forward`.

**A:**

| | `std::move` | `std::forward<T>` |
|---|-----------|-------------------|
| Purpose | Cast to rvalue (enable move) | Preserve value category (perfect forwarding) |
| When | You KNOW you want to move | In template, forwarding to another function |
| Result | Always rvalue reference | rvalue if passed rvalue, lvalue if passed lvalue |

```cpp
// std::move — used in EventFactory:
Event e;
e.payload = std::move(payload);  // payload is now empty

// std::forward — would be used in a generic push:
template<typename T>
void push(T&& item) {
    queue_.emplace_back(std::forward<T>(item));
    // If item was an rvalue → moves into queue
    // If item was an lvalue → copies into queue
}
```

**Common mistake:** Using `std::move` on a `const` object does nothing — `const Event&&` can't bind to a move constructor (which takes `Event&&`). The copy constructor is called instead. Silently slow.

---

### Q126. What is `std::variant` and could it replace the priority enum?

**A:** `std::variant` is a type-safe union:

```cpp
using EventData = std::variant<RealtimePayload, TransactionalPayload, BatchPayload>;

struct Event {
    EventData data;
    // ...
};

// Processing via std::visit:
std::visit(overloaded{
    [](const RealtimePayload& p) { /* realtime processing */ },
    [](const TransactionalPayload& p) { /* transactional processing */ },
    [](const BatchPayload& p) { /* batch processing */ },
}, event.data);
```

**Pros:** Type safety — each processor gets its own payload type. Compile-time exhaustiveness checking.
**Cons:** `std::visit` has overhead (function pointer dispatch or jump table). The project's enum + switch is simpler and faster for this use case.

---

## S — Trade-offs & "What Would You Change?"

### Q127. What is the biggest weakness of this architecture?

**A:** **Thread-per-connection in IngestServer.** It limits concurrent connections to ~1K (stack memory) and doesn't scale for modern cloud deployments.

**Fix:** Replace with `epoll` event loop + thread pool. One thread handles all connections; when data is ready, it dispatches to a worker pool for parsing. This scales to 100K+ connections with ~4 threads.

---

### Q128. Why not use a message broker (Kafka, RabbitMQ) instead of the MPSC queue?

**A:**

| Aspect | In-process MPSC | Kafka |
|--------|----------------|-------|
| Latency | ~20 ns | ~1 ms (network + serialization) |
| Throughput | 50M events/sec | ~1M events/sec |
| Durability | None (process-local) | Full (replicated disk) |
| Scaling | Single process | Multi-process, multi-datacenter |
| Complexity | 100 lines of code | Zookeeper cluster, broker cluster |

**The project's MPSC is 50,000× faster.** The trade-off is zero durability — a crash loses all in-flight events. For an edge/embedded use case (like CCU2), this is acceptable. For a cloud service, Kafka would be the right choice.

---

### Q129. If you had to rewrite this in Rust, what would change?

**A:**

| C++ Construct | Rust Equivalent | What Changes |
|--------------|----------------|-------------|
| `std::shared_ptr<Event>` | `Arc<Event>` | Same semantics, but Rust prevents data races at compile time |
| `std::atomic<uint64_t>` | `AtomicU64` | Same API |
| `std::mutex<T>` + manual lock | `Mutex<T>` — data is INSIDE the mutex | Can't access data without locking — eliminates a class of bugs |
| `std::thread` | `std::thread::spawn` | Must prove sent data is `Send` — compile-time thread safety |
| Custom deleter | `Drop` trait | Automatic, no custom deleter syntax needed |
| `unsafe` FFI | Same | C bridge would use `unsafe` blocks — explicitly marked |

**Main benefit:** The Rust compiler would catch the `ProcessedEventStream::observers_` vector access pattern (shared mutable state) at compile time — requiring explicit synchronization before it compiles.

---

### Q130. What would you change about the DLQ design?

**A:**

| Current | Improved |
|---------|----------|
| In-memory only | Persistent (append-only file) |
| Fixed capacity (1000) | Configurable, with back-pressure to caller |
| No replay | Replay with exponential backoff |
| Single queue | Per-topic DLQ for targeted replay |
| No metadata | Include full exception trace, retry count, origin processor |
| No alerting | Alert when DLQ size exceeds threshold |

---

### Q131. How would you add event ordering guarantees?

**A:** Currently, events with the same topic can be processed out of order if they go through different processors.

**Solution — partition by topic:**
1. Hash topic to a partition number: `partition = hash(topic) % NUM_PARTITIONS`.
2. Each partition has its own queue and processor thread.
3. Events with the same topic always go to the same partition → FIFO order guaranteed.

**Trade-off:** Reduces parallelism (hot topics create bottleneck partitions). Mitigation: dynamic partition rebalancing or sub-topic partitioning.

---

### Q132. Why store events in a custom binary format instead of JSON/Protobuf?

**A:**

| Format | Serialization Cost | Size | Schema Evolution |
|--------|-------------------|------|------------------|
| Custom binary | ~0 (memcpy) | Minimal | Hard (manual versioning) |
| JSON | ~1μs (string formatting) | 2-3× larger | Easy (add fields) |
| Protobuf | ~100ns (wire format) | ~1.3× of binary | Built-in (field numbers) |
| FlatBuffers | ~0 (zero-copy) | ~1.1× of binary | Good (schema evolution) |

The project uses custom binary for maximum ingest speed. For production, **FlatBuffers** would be ideal — zero-copy like custom binary, but with schema evolution support.

---

### Q133. What if you needed exactly-once delivery across process restarts?

**A:** Currently, events can be lost on crash (in-memory queues are gone) or duplicated on retry (TransactionalProcessor retries, but the first attempt might have partially succeeded).

**Solution — write-ahead log (WAL):**
1. On ingest, append event to a durable WAL file before entering the MPSC queue.
2. On successful processing, mark event as complete in the WAL.
3. On restart, replay uncommitted events from the WAL.
4. The dedup CAS map prevents duplicates from replay.

**Cost:** One `fsync` per event (or per batch for higher throughput). This adds ~100μs per event — acceptable for transactional workloads, too slow for realtime.

---

### Q134. How would you add multi-tenancy?

**A:**

1. **Namespace topics:** `tenant_id/topic_name` (e.g., `acme/orders`).
2. **Per-tenant resource limits:** Max events/sec, max queue depth, max DLQ size.
3. **Tenant isolation:** Separate SPSC queues per tenant for the realtime path, preventing one tenant's burst from affecting others.
4. **Authentication:** Token includes tenant ID, validated in `parseFrame()`.

---

### Q135. What monitoring would you add for production?

**A:**

```
Essential metrics:
  ✅ Already have:
    - event_count (per processor)
    - drop_count
    - dlq_size
    - pressure_level
    - latency_histogram

  ❌ Need to add:
    - connections_active (current TCP connections)
    - connections_total (cumulative)
    - queue_depth (per queue, sampled every second)
    - parse_errors (malformed frames)
    - bytes_ingested_total
    - gc_pool_hits / gc_pool_misses (pool efficiency)
    - thread_cpu_time (per-thread CPU usage via getrusage)
    - fd_count (open file descriptors — detect leaks)
    - memory_rss (resident set size — detect memory leaks)

Alerting rules:
  - pressure_level >= CRITICAL for > 60s → page on-call
  - dlq_size > 800 (80% capacity) → warning
  - parse_errors > 100/min → possible attack or protocol mismatch
  - latency_p99 > 10ms for realtime → SLA breach
  - connections_active > 500 → approaching thread limit
```

---

## T — Production & Deployment Readiness

### Q136. How would you containerize this project?

**A:**

```dockerfile
# Multi-stage build
FROM ubuntu:22.04 AS builder
RUN apt-get update && apt-get install -y cmake g++ libnuma-dev libyaml-cpp-dev
COPY . /src
WORKDIR /src/build
RUN cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y libnuma1 libyaml-cpp0.7
COPY --from=builder /src/build/eventstream_core /usr/local/bin/
COPY --from=builder /src/build/libesccore.so /usr/local/lib/
COPY config/ /etc/eventstream/
EXPOSE 8080/tcp 8081/udp
ENTRYPOINT ["eventstream_core", "--config", "/etc/eventstream/config.yaml"]
```

**Multi-stage build:** Builder stage has compiler + dev libraries (large). Final stage has only runtime libraries (small). Typical image size: ~50 MB vs ~500 MB with single-stage.

---

### Q137. How would you implement health checks?

**A:** Add an HTTP health endpoint:

```cpp
// GET /health
{
    "status": "healthy",
    "uptime_seconds": 3600,
    "pressure_level": "HEALTHY",
    "queue_fill_percent": 23.5,
    "events_per_second": 150000,
    "dlq_size": 12,
    "threads_active": 9
}
```

**Kubernetes probes:**
```yaml
livenessProbe:
  httpGet:
    path: /health
    port: 9090
  initialDelaySeconds: 5
  periodSeconds: 10

readinessProbe:
  httpGet:
    path: /ready
    port: 9090
  # Returns 503 if pressure_level >= CRITICAL
```

---

### Q138. How would you implement graceful drain for rolling updates?

**A:**

1. Kubernetes sends `SIGTERM`.
2. Signal handler sets `g_running = false` and `pipelineState = DRAINING`.
3. IngestServer stops accepting new connections.
4. Existing connections continue until their current frame is processed.
5. Dispatcher drains the MPSC queue completely.
6. Processors finish their current events.
7. StorageEngine flushes and closes files.
8. Process exits with code 0.

**Kubernetes `terminationGracePeriodSeconds`:** Set to 30 seconds. If the process doesn't exit in 30s, Kubernetes sends `SIGKILL`. The project should aim to drain within 10 seconds.

---

### Q139. How do you handle configuration changes without restart?

**A:** Two approaches:

1. **SIGHUP reload:** Handler sets a flag, AdminLoop checks it and reloads `config.yaml`:
```cpp
void sighupHandler(int) {
    g_reload_config.store(true, std::memory_order_relaxed);
}
```

2. **File watch:** Use `inotify` to watch `config.yaml` for changes:
```cpp
int fd = inotify_init();
inotify_add_watch(fd, "/etc/eventstream/config.yaml", IN_MODIFY);
// In AdminLoop: poll(fd) → if modified → reload
```

**What can be reloaded safely:**
- TopicTable mappings (under unique_lock) ✅
- Pressure thresholds ✅
- Log level ✅

**What requires restart:**
- Listening port (socket already bound)
- Queue capacities (fixed at allocation time)
- Number of processor threads

---

### Q140. What is the observability stack for this project?

**A:**

```
EventStreamCore → /metrics endpoint (Prometheus format)
       ↓
  Prometheus (scrapes every 15s)
       ↓
  Grafana (dashboards, alerting)
       ↓
  PagerDuty (on-call notifications)

Logs:
  spdlog → stdout → container runtime → Fluentd/Loki → Grafana

Traces (future):
  OpenTelemetry SDK → Jaeger/Tempo
  Trace each event from ingest to storage with span IDs
```

---

### Q141. How do you handle log rotation?

**A:** Three options:

1. **Stdout logging** (current): Container runtime handles rotation. `docker logs --since 1h` for recent logs. Kubernetes uses `logrotate` on node.

2. **spdlog rotating file sink:**
```cpp
auto logger = spdlog::rotating_logger_mt("esc", "logs/esc.log",
    1048576 * 100,  // 100 MB max file size
    3);              // Keep 3 rotated files
```

3. **syslog:** `spdlog::syslog_logger` sends to system syslog daemon, which handles rotation.

**For production containers:** Option 1 (stdout) is standard. The orchestrator (Kubernetes) handles collection and rotation.

---

### Q142. What is a canary deployment and how does it apply here?

**A:** Deploy the new version to a small subset (5%) of traffic, monitor metrics, then gradually increase to 100%.

**For EventStreamCore:**
1. Deploy new version as a separate pod.
2. Load balancer sends 5% of connections to the canary.
3. Monitor: latency_p99, error_rate, DLQ growth, pressure_level.
4. If metrics are within SLA for 15 minutes → increase to 25%, then 50%, then 100%.
5. If any metric degrades → automatic rollback to previous version.

**Key metric for canary:** `events_dropped_total` — if the new version drops more events than the old, rollback immediately.

---

## Quick-Fire Round (Rapid Answers)

| Q | A |
|---|---|
| What's the default SPSC capacity? | 16384 (2¹⁴) |
| What's the default MPSC capacity? | 65536 (2¹⁶) |
| How many dedup buckets? | 4096 |
| Dedup expiry window? | 1 hour |
| Batch flush interval? | 5 seconds |
| Retry backoff sequence? | 100ms, 200ms, 400ms |
| Max retry count? | 3 |
| DLQ capacity? | 1000 |
| Admin snapshot interval? | 10 seconds |
| HighPerformanceEvent size? | 576 bytes (9 cache lines) |
| CRC polynomial? | 0xEDB88320 (reflected Ethernet) |
| Wire protocol overhead? | 7 + topic_len bytes |
| Number of pressure levels? | 5 |
| MetricSnapshot cache TTL? | 100 ms |
| LatencyHistogram buckets? | ~21 (log₂ scale) |
| futex uncontended cost? | ~25ns (no syscall) |
| epoll_wait complexity? | O(ready fds) |
| context switch cost? | ~2-5 μs |
| cache line size (x86)? | 64 bytes |
| MFENCE cost? | ~20-40 cycles |
| page fault (minor)? | ~1 μs |
| page fault (major SSD)? | ~100 μs |
| `std::shared_ptr` size? | 16 bytes (2 pointers) |
| cgo call overhead? | ~50-100 ns |
| TCP TIME_WAIT duration? | 60 seconds (Linux) |
| `std::string` SBO threshold? | 15-22 bytes (implementation-dependent) |
| `std::make_shared` allocations? | 1 (combined) vs 2 (separate) |
| TLS handshake cost? | ~1 ms (RSA-2048) |
| `steady_clock::now()` cost? | ~20 ns (vDSO) |
| Branch misprediction penalty? | ~15-20 cycles |
| ASan slowdown? | ~2× |
| TSan slowdown? | ~5-15× |
| Docker multi-stage image reduction? | ~10× (500MB → 50MB) |
| Kubernetes `SIGTERM` grace period? | Default 30 seconds |
| Prometheus scrape interval? | Default 15 seconds |
| Max TCP connections (thread-per-conn)? | ~1K (stack memory limited) |
| Max TCP connections (epoll)? | ~100K+ |
