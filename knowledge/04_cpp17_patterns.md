# 04 — Modern C++17 Patterns

> **Goal:** Identify every C++17 (and modern C++) feature used in EventStreamCore and understand *why* it was chosen.

---

## 1. `std::optional<T>` — Nullable Value Without Pointers

**Used in:** SPSC pop, MPSC pop, EventBus pop, Dispatcher tryPop, MetricRegistry getSnapshot

### What It Replaces

```cpp
// Old C++ style:
bool pop(T& out);                  // awkward out-parameter
T* pop();                          // who owns the pointer?

// C++17 style:
std::optional<T> pop();            // clear semantics: value or nothing
```

### Project Examples

```cpp
// SpscRingBuffer::pop()
std::optional<T> pop() {
    if (tail == head) return std::nullopt;   // empty
    T item = buffer_[tail];
    return item;                              // implicit conversion to optional<T>
}

// Caller (Dispatcher)
auto event_opt = inbound_queue_.pop();
if (!event_opt) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    continue;
}
auto queueId = route(event_opt.value());  // .value() extracts the contained value
```

### Interview Point

`std::optional` has **no heap allocation** — the value is stored inline (stack or member). Unlike `std::unique_ptr`, there's no `new`/`delete` overhead. Unlike `std::pair<bool, T>`, the value is only constructed when present.

---

## 2. `std::string_view` — Non-Owning String Reference

**Used in:** MetricRegistry, MetricNames constants

### What It Replaces

```cpp
// Old: string literals become std::string (heap allocation!)
const std::string EVENTBUS = "EventBusMulti";

// C++17: zero-copy view over a string literal
constexpr std::string_view EVENTBUS = "EventBusMulti";
```

### Project Usage

```cpp
namespace MetricNames {
    constexpr std::string_view EVENTBUS        = "EventBusMulti";
    constexpr std::string_view REALTIME        = "RealtimeProcessor";
    constexpr std::string_view TRANSACTIONAL   = "TransactionalProcessor";
    constexpr std::string_view BATCH           = "BatchProcessor";
}

// MetricRegistry accepts string_view directly:
Metrics& getMetrics(std::string_view name);
```

### Why It Matters

`string_view` is a `{const char*, size_t}` pair — 16 bytes, no allocation. Passing `MetricNames::REALTIME` to `getMetrics()` doesn't create a `std::string` temporary. Inside `getMetrics()`, the `std::string` is only constructed when inserting into the map.

### Gotcha

`string_view` does **not** own the underlying data. Never store a `string_view` to a temporary:

```cpp
std::string_view bad = std::string("temp");  // dangling!
```

---

## 3. Structured Bindings — Decompose Aggregates

**Used in:** MetricRegistry, ConfigLoader, BatchProcessor, Admin

### Syntax

```cpp
// C++17 structured binding
auto [it, inserted] = metrics_map_.try_emplace(name);

// Equivalent C++11:
auto result = metrics_map_.try_emplace(name);
auto it = result.first;
bool inserted = result.second;
```

### Project Examples

```cpp
// MetricRegistry::getMetrics()
auto [it, inserted] = metrics_map_.try_emplace(std::string(name));
return it->second;

// BatchProcessor::process()
auto [it, inserted] = buckets_.try_emplace(event.topic);
TopicBucket& bucket = it->second;

// Admin::loop()
for (const auto& [name, snap] : snapshots) {
    total_processed += snap.total_events_processed;
}
```

### Why It's Better

1. **Readability** — `[name, snap]` is self-documenting.
2. **No temporary pair** — the compiler binds directly to map entries.
3. **Works with tuples, pairs, aggregates, and user types** (if `get<>` is specialized).

---

## 4. `if constexpr` — Compile-Time Branching

**Used in:** EventPool (SFINAE pool_index detection)

### The Problem

```cpp
// This won't compile if EventType doesn't have pool_index_:
if (kHasPoolIndex) {
    idx = obj->pool_index_;  // ERROR: member doesn't exist!
}
```

### The Solution

```cpp
if constexpr (kHasPoolIndex) {
    idx = obj->pool_index_;     // only compiled if kHasPoolIndex is true
} else {
    auto it = ptr_to_index_.find(obj);  // only compiled if false
}
```

`if constexpr` discards the dead branch **before** template instantiation, so the compiler never sees the invalid member access.

### When to Use

| Scenario | Use |
|----------|-----|
| Type-dependent code in templates | `if constexpr` |
| Runtime condition | regular `if` |
| SFINAE overload selection | `enable_if_t` or concepts (C++20) |

---

## 5. `inline` Variables — Header-Only Globals

**Used in:** MetricNames constants (implicitly via `constexpr`)

### Background

Before C++17, defining a global variable in a header caused **multiple definition errors**:

```cpp
// pre-C++17: ODR violation if included in multiple TUs
const std::string NAME = "hello";  // each TU gets its own copy!
```

C++17's `inline` variables (and `constexpr` which implies `inline`) solve this:

```cpp
constexpr std::string_view NAME = "hello";  // one definition across all TUs
```

---

## 6. `std::shared_mutex` — Reader-Writer Lock

**Used in:** TopicTable

### Why Not Just `std::mutex`?

TopicTable is read on every event dispatch but written only on config reload:

```cpp
// Reader (hot path — called millions of times):
bool findTopic(const std::string& topic, EventPriority& priority) const {
    std::shared_lock lock(mutex_);    // multiple readers concurrently
    auto it = table_.find(topic);
    ...
}

// Writer (cold path — called once on startup):
bool loadFromFile(const std::string& path) {
    std::unique_lock lock(mutex_);    // exclusive access
    ...
}
```

`std::shared_lock` allows **N readers** simultaneously. `std::unique_lock` blocks all readers and other writers.

### Performance Impact

| Readers | std::mutex | std::shared_mutex |
|---------|-----------|-------------------|
| 1 | ~25ns | ~30ns (slightly slower due to rwlock overhead) |
| 4 | ~25ns × 4 serial | ~30ns total (parallel reads!) |
| 8 | ~25ns × 8 serial | ~30ns total |

For a read-dominated workload, `shared_mutex` scales linearly with reader count.

---

## 7. `std::chrono` — Type-Safe Time

**Used everywhere** — timeouts, timestamps, latency measurement, batch windows.

### Key Types

```cpp
using Clock = std::chrono::steady_clock;          // monotonic, won't jump
using Clock = std::chrono::high_resolution_clock;  // highest resolution available
using Clock = std::chrono::system_clock;           // wall-clock, can jump (NTP)

// Duration literals (C++14 but used throughout):
using namespace std::chrono_literals;
auto timeout = 100ms;    // std::chrono::milliseconds(100)
auto window  = 5s;       // std::chrono::seconds(5)
```

### Project Usage Patterns

```cpp
// Latency measurement (RealtimeProcessor):
auto start = std::chrono::high_resolution_clock::now();
handle(event);
auto end = std::chrono::high_resolution_clock::now();
auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

// Batch window check (BatchProcessor):
if (now - bucket.last_flush_time >= window_) {
    flushBucketLocked(bucket, topic);
}

// Nanosecond timestamps (Event):
inline uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}
```

---

## 8. `std::call_once` / `std::once_flag` — Thread-Safe Initialization

**Used in:** EventFactory CRC-32 table initialization

```cpp
uint32_t EventFactory::calculateCRC32(const std::vector<uint8_t>& data) {
    static uint32_t CRC32_TABLE[256];
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
                else crc >>= 1;
            }
            CRC32_TABLE[i] = crc;
        }
    });
    // ... use table ...
}
```

`call_once` guarantees the lambda runs exactly once, even if multiple threads call `calculateCRC32()` simultaneously. After the first call, subsequent calls are a single atomic check (nearly free).

---

## 9. Move Semantics — Zero-Copy Event Construction

**Used in:** EventFactory, Event constructor, IngestEventPool

```cpp
Event EventFactory::createEvent(
    EventSourceType sourceType,
    EventPriority priority,
    std::vector<uint8_t>&& payload,     // rvalue reference: caller moves data in
    std::string&& topic,
    std::unordered_map<std::string,std::string>&& metadata
) {
    // ... build header ...
    return Event(h, std::move(topic), std::move(payload), std::move(metadata));
}
```

**Why `&&`?** The caller transfers ownership of the vector/string without copying:

```cpp
// Caller:
auto evt = EventFactory::createEvent(
    EventSourceType::TCP, EventPriority::HIGH,
    std::move(payload_vec),    // payload_vec is now empty
    std::move(topic_str),      // topic_str is now empty  
    std::move(metadata_map)
);
```

For a 64KB payload, this saves a 64KB `memcpy` + heap allocation.

---

## 10. `thread_local` — Per-Thread State

**Used in:** EventBus NUMA binding, processor NUMA binding, MetricRegistry timestamp buffer

```cpp
// EventBus::pop() — bind thread to NUMA node on first call
static thread_local bool bound = false;
if (!bound && numa_node_ >= 0) {
    NUMABinding::bindThreadToNUMANode(numa_node_);
    bound = true;
}

// MetricRegistry::updateEventTimestamp() — throttle per-thread
thread_local struct {
    std::string last_name;
    uint64_t last_update_ns = 0;
} buffer;
```

`thread_local` variables are allocated once per thread (not per call). They're stored in Thread-Local Storage (TLS) — a segment of memory unique to each thread, accessed via a fast register (`%fs` on x86-64).

---

## 11. Enum Classes — Scoped and Type-Safe

**Used everywhere:** `EventPriority`, `EventSourceType`, `QueueId`, `OverflowPolicy`, `PressureLevel`, `PipelineState`, `ControlAction`, `FailureState`, `AlertLevel`, `HealthStatus`

```cpp
enum struct EventPriority {
    BATCH = 0, LOW = 1, MEDIUM = 2, HIGH = 3, CRITICAL = 4
};

// Usage — must qualify:
evt.header.priority = EventPriority::HIGH;

// Won't compile:
int p = EventPriority::HIGH;  // no implicit conversion to int
int p = static_cast<int>(EventPriority::HIGH);  // explicit cast required
```

**Advantages over plain `enum`:**
1. **No namespace pollution** — `HIGH` doesn't conflict with other enums.
2. **No implicit int conversion** — prevents `if (priority == 3)` bugs.
3. **Forward-declarable** — can reduce header dependencies.

---

## 12. Summary Table

| Feature | C++ Version | Where Used | Why |
|---------|------------|------------|-----|
| `std::optional` | C++17 | Queue pop, Dispatcher tryPop | Nullable return without pointers |
| `std::string_view` | C++17 | MetricNames, MetricRegistry | Zero-copy string references |
| Structured bindings | C++17 | map iteration, try_emplace | Readable decomposition |
| `if constexpr` | C++17 | EventPool SFINAE | Compile-time branch elimination |
| `inline` variables | C++17 | constexpr globals | ODR-safe header constants |
| `std::shared_mutex` | C++17 | TopicTable | Read-heavy concurrent access |
| `std::chrono` | C++11+ | All timing code | Type-safe time representation |
| `std::call_once` | C++11 | CRC-32 table init | Thread-safe one-time init |
| Move semantics | C++11 | Event creation, pool | Zero-copy data transfer |
| `thread_local` | C++11 | NUMA binding, metrics | Per-thread state, no locking |
| `enum class` | C++11 | All enums | Scoped, type-safe enumerations |
| `alignas(N)` | C++11 | Queues, events | Cache-line alignment |
| `static_assert` | C++11 | Queue capacity, event size | Compile-time invariant checks |

---

## 13. Deep Dive: Template Metaprogramming in EventPool

### 13.1 The SFINAE Detection Pattern — Step by Step

```cpp
// Attempt 1: check if T has a member called pool_index_
template<typename T>
static constexpr auto has_pool_index(int)
    -> decltype(std::declval<T>().pool_index_, std::true_type{}) {
    return {};
}

// Attempt 2: fallback if Attempt 1 fails
template<typename T>
static constexpr std::false_type has_pool_index(...) {
    return {};
}
```

**How the compiler resolves this:**

1. `has_pool_index<MyEvent>(0)` — the argument is `int`.
2. Compiler tries overload 1 first (exact match for `int` parameter):
   - Evaluates `decltype(std::declval<MyEvent>().pool_index_, ...)`.
   - If `MyEvent` has `pool_index_`: substitution succeeds → returns `std::true_type`.
   - If `MyEvent` lacks `pool_index_`: substitution **fails** → SFINAE removes this overload.
3. If overload 1 was removed: compiler falls back to overload 2 (`...` accepts anything) → returns `std::false_type`.

**Why `std::declval<T>()`?** It produces a "fake" reference to `T` without constructing it. We only need the type expression to compile — we never run it. This works even if `T` has no default constructor.

**Why the comma operator `(expr, std::true_type{})`?** The comma operator evaluates both sides but returns the last. This discards the type of `pool_index_` (we don't care what type it is) and returns `std::true_type`.

### 13.2 if constexpr — The "Static If"

```cpp
static constexpr bool kHasPoolIndex = decltype(has_pool_index<EventType>(0))::value;

void release(EventType* obj) {
    if constexpr (kHasPoolIndex) {
        size_t idx = obj->pool_index_;      // Only compiled if true
    } else {
        auto it = ptr_to_index_.find(obj);  // Only compiled if false
    }
}
```

**Without `if constexpr` (C++14):** Both branches compile for every template instantiation. If `EventType` doesn't have `pool_index_`, `obj->pool_index_` is a hard compile error — even inside `if (false)`.

**With `if constexpr` (C++17):** The false branch is **discarded** — not compiled at all. The compiler doesn't even check if the names resolve. This is the same effect as `#ifdef` but type-safe and works with template parameters.

### 13.3 The `static_assert` Power-of-2 Check

```cpp
static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
```

**Bit trick explanation:**
- A power of 2 has exactly one bit set: `1000...0`.
- `Capacity - 1` has all lower bits set: `0111...1`.
- `AND` of these = 0 (no overlapping bits).
- For non-powers of 2 (e.g., 6 = `110`, 5 = `101`): `110 & 101 = 100 ≠ 0`.

**Why at compile time?** Catching this at runtime (via assert) means the error only appears when the code path is hit. `static_assert` fails during compilation — the bug can't reach production.

---

## 14. Deep Dive: Perfect Forwarding and Universal References

### 14.1 EventFactory Parameter Design

```cpp
static Event createEvent(EventSourceType sourceType,
                         EventPriority priority,
                         std::vector<uint8_t>&& payload,    // rvalue ref
                         std::string&& topic,               // rvalue ref
                         std::unordered_map<...>&& metadata); // rvalue ref
```

These are **rvalue references** (not universal references — `T&&` is universal only when `T` is deduced). The `&&` forces callers to move:

```cpp
// Must use std::move:
auto evt = EventFactory::createEvent(src, pri,
    std::move(payload), std::move(topic), std::move(meta));

// This won't compile:
auto evt = EventFactory::createEvent(src, pri,
    payload, topic, meta);  // ERROR: can't bind lvalue to rvalue ref
```

**Why not `const T&` (copy)?** Payload can be 10KB+. Copying wastes time and memory. Move transfers ownership in O(1) — just pointer swap.

**Why not `T&&` with template deduction (universal ref)?** No need — the factory always moves into the Event. Universal references add complexity for no benefit here.

### 14.2 The std::move Misconception

`std::move` does NOT move anything. It's a **cast**:

```cpp
template<typename T>
constexpr std::remove_reference_t<T>&& move(T&& arg) noexcept {
    return static_cast<std::remove_reference_t<T>&&>(arg);
}
```

It casts `arg` to an rvalue reference. The actual move happens when the move constructor/assignment is invoked:

```cpp
e.topic = std::move(topic);  
// Step 1: std::move(topic) → cast to string&&
// Step 2: string::operator=(string&&) is called
// Step 3: Swaps internal pointers (O(1), no allocation)
// Step 4: topic is now in a "moved-from" state (valid but unspecified)
```

---

## 15. Deep Dive: shared_mutex vs mutex Performance

### 15.1 When to Use Which

```
Time → ═══════════════════════════════════════════

std::mutex:            [Reader1]  [Reader2]  [Reader3]  [Writer1]
                       serialized — one at a time

std::shared_mutex:     [Reader1][Reader2][Reader3]        [Writer1]
                       ├── concurrent ──┤               ├─ exclusive ─┤

Benefit: N readers run in PARALLEL instead of SERIAL
```

### 15.2 Internal Implementation (Linux, glibc)

`std::shared_mutex` on Linux wraps `pthread_rwlock_t`:

```
struct pthread_rwlock {
    unsigned int __readers;       // count of active readers
    unsigned int __writers;       // count of waiting writers
    unsigned int __wrphase_futex; // futex for writer phase
    ...
};

shared_lock():  atomic_inc(__readers); if (__writers > 0) → futex_wait
unique_lock():  while (__readers > 0) → futex_wait; __writers = 1
```

**Cost per shared_lock:** ~25ns (atomic increment + one branch). 
**Cost per unique_lock:** ~50ns uncontended, but blocks if any readers are active.

### 15.3 Read-Write Ratio Breakeven

| Read:Write Ratio | shared_mutex Speedup |
|-------------------|---------------------|
| 1:1 | 0.8× (slower — overhead of reader counting) |
| 10:1 | 2-3× |
| 100:1 | 5-10× |
| 1000:1 | Near-linear with reader count |

TopicTable in the project: ~1M reads/sec (every event routes through `findTopic`) vs ~1 write/hour (config reload). Ratio: ~3.6 billion : 1. `shared_mutex` is overwhelmingly correct here.

---

## 16. Deep Dive: std::chrono Type Safety

### 16.1 Why Not Just Use `int`?

```cpp
// Dangerous:
void sleep(int ms);
sleep(5);        // 5 what? Milliseconds? Seconds? Microseconds?
sleep(5000);     // Someone assumed seconds, used milliseconds

// Type-safe:
void sleep(std::chrono::milliseconds ms);
sleep(5ms);               // clear
sleep(5s);                // converts automatically: 5000ms
sleep(5us);               // compile error? No — converts down
```

### 16.2 Zero-Cost Abstraction

`std::chrono::milliseconds` is a `duration<int64_t, std::ratio<1, 1000>>`. At runtime, it's just an `int64_t` — zero overhead. The type system catches unit errors at compile time. There's no virtual dispatch, no heap allocation, no runtime conversion — pure compile-time safety.

### 16.3 The `nowNs()` Helper in the Project

```cpp
inline uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}
```

**`high_resolution_clock`** maps to `clock_gettime(CLOCK_MONOTONIC)` on Linux → reads the CPU's Time Stamp Counter (TSC) via `rdtsc` instruction → ~20ns call. Monotonic means it never goes backward (immune to NTP adjustments).

---

## 17. Extended Interview Questions

**Q1: What is ADL (Argument-Dependent Lookup) and does this project use it?**  
A: ADL means the compiler searches the namespaces of a function's argument types for matching functions. In the project, `spdlog::info("...", EventPriority::HIGH)` works because `spdlog` provides formatters found via ADL. Also, `std::move(topic)` finds `std::move` via ADL on the `std::string` argument.

**Q2: What is the Rule of Five? Does `Event` follow it?**  
A: The Rule of Five: if you define one of {destructor, copy constructor, copy assignment, move constructor, move assignment}, define all five. `Event` relies on compiler-generated versions (= default) because all its members (`string`, `vector`, `map`) have correct move/copy semantics. No custom destructor needed → no Rule of Five violation.

**Q3: What is a "moved-from" state?**  
A: After `std::move(x)`, `x` is in a "valid but unspecified" state. For `std::string`, this means it's either empty or contains some value. You can call `.size()`, `.clear()`, or assign to it, but you can't rely on its contents. The project's `*e = Event{}` reset explicitly puts the moved-from Event back into a known state.

**Q4: Why `constexpr` for MetricNames instead of `const`?**  
A: `constexpr std::string_view` is computed at compile time and stored in the read-only `.rodata` section. `const std::string` would be constructed at program startup (static initialization) → potential SIOF. `constexpr` has zero runtime cost.

**Q5: What is the "static initialization order fiasco" (SIOF)?**  
A: Global variables in different translation units have no guaranteed initialization order. If global A depends on global B but B hasn't been constructed yet → UB. The project avoids SIOF by using Meyers' singletons (`static` local in a function) and `constexpr` constants, both of which have guaranteed initialization order.

**Q6: Explain `[[nodiscard]]` and when you'd use it.**  
A: `[[nodiscard]]` makes the compiler warn if the return value is ignored. Used on factory methods (`createEvent`) and pool acquire (`acquireEvent`) — ignoring the returned event is always a bug (memory leak or lost event). Not used on `push()` because callers sometimes intentionally ignore push failures.
