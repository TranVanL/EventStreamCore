# 10 — Design Patterns

> **Goal:** Identify every design pattern in EventStreamCore and explain how and why it's used.

---

## 1. Pattern Catalogue

| Pattern | Category | Where Used |
|---------|----------|-----------|
| **Singleton** | Creational | MetricRegistry, ProcessedEventStream |
| **Object Pool** | Creational | IngestEventPool, EventPool |
| **Factory Method** | Creational | EventFactory::createEvent() |
| **Observer** | Behavioral | ProcessedEventStream, AlertHandler |
| **Strategy** | Behavioral | OverflowPolicy, AlertHandler hierarchy |
| **Template Method** | Behavioral | EventProcessor::process() |
| **Composite** | Structural | CompositeAlertHandler |
| **Null Object** | Behavioral | NullAlertHandler |
| **RAII** | Idiom | Mutex locks, file handles, thread joining |
| **Pipeline** | Architectural | Ingest → Dispatch → Bus → Process → Store |
| **Producer-Consumer** | Concurrency | MPSC queue, EventBus queues |
| **Backpressure** | Concurrency | 5-level cascade, PipelineState |

---

## 2. Singleton — MetricRegistry

```cpp
class MetricRegistry {
public:
    static MetricRegistry& getInstance() {
        static MetricRegistry instance;  // Meyers' Singleton
        return instance;
    }
private:
    MetricRegistry() = default;
    MetricRegistry(const MetricRegistry&) = delete;
    MetricRegistry& operator=(const MetricRegistry&) = delete;
};
```

**Why Singleton?** There must be exactly one registry aggregating metrics from all components. Multiple registries would give inconsistent snapshots.

**Thread safety:** C++11 guarantees `static` local initialization is thread-safe. The first thread to call `getInstance()` initializes it; all others wait.

**Why Meyers' style?** Avoids the static initialization order fiasco (SIOF) — the singleton is created on first use, not at program startup when other globals might not exist yet.

**Also used by:** `ProcessedEventStream::getInstance()`

---

## 3. Object Pool — IngestEventPool

```cpp
class IngestEventPool {
    static std::queue<std::unique_ptr<Event>>& getPool() {
        static std::queue<std::unique_ptr<Event>> pool;
        return pool;
    }

    static std::shared_ptr<Event> acquireEvent() {
        // Pop from pool or allocate from heap
        // Return shared_ptr with CUSTOM DELETER that returns to pool
    }
};
```

**Pattern intent:** Avoid frequent allocation/deallocation of expensive objects.

**Custom deleter as the "return" mechanism:** When the `shared_ptr` reference count reaches zero, instead of `delete`, the custom deleter pushes the object back into the pool. This is transparent to the user — they just let the `shared_ptr` go out of scope.

**Template variant:** `EventPool<EventType, Capacity>` uses SFINAE to detect `pool_index_` and provides O(1) acquire/release via a free-list stack.

---

## 4. Factory Method — EventFactory

```cpp
class EventFactory {
public:
    static Event createEvent(
        EventSourceType sourceType,
        EventPriority priority,
        std::vector<uint8_t>&& payload,
        std::string&& topic,
        std::unordered_map<std::string,std::string>&& metadata
    );
private:
    static uint32_t calculateCRC32(const std::vector<uint8_t>& data);
    static std::atomic<uint64_t> global_event_id;
};
```

**Pattern intent:** Encapsulate object creation logic.

**Why a factory?** Creating an Event requires:
1. Generating a unique ID (atomic increment).
2. Computing CRC-32 of the payload.
3. Capturing a nanosecond timestamp.

Without a factory, every callsite would need to duplicate this logic.

---

## 5. Observer — ProcessedEventStream

```cpp
class ProcessedEventObserver {
    virtual void onEventProcessed(const Event& event, const char* processor_name) = 0;
    virtual void onEventDropped(const Event& event, const char* processor_name, const char* reason) = 0;
};

class ProcessedEventStream {
    std::vector<ProcessedEventObserverPtr> observers_;

    void notifyProcessed(const Event& event, const char* processor_name) {
        auto copy = observers_;  // snapshot
        for (auto& obs : copy) {
            obs->onEventProcessed(event, processor_name);
        }
    }
};
```

**Pattern intent:** Decouple event producers from consumers. Processors don't know who is listening.

**Snapshot copy:** Prevents invalidation if an observer unsubscribes during notification.

**Kill switch:** `std::atomic<bool> enabled_` allows disabling all notifications for performance testing.

**Used by:** C API bridge (subscribes to forward events to SDK callbacks).

---

## 6. Strategy — OverflowPolicy

```cpp
enum class OverflowPolicy : int {
    DROP_OLD = 0,         // Evict oldest event
    BLOCK_PRODUCER = 1,   // Wait for space
    DROP_NEW = 2          // Reject incoming event
};
```

Each EventBus queue has a configurable overflow policy:

```cpp
switch (queue->policy) {
    case OverflowPolicy::BLOCK_PRODUCER:
        queue->cv.wait_for(lock, 100ms, ...);
        break;
    case OverflowPolicy::DROP_NEW:
        return false;
    case OverflowPolicy::DROP_OLD:
        dlq_.push(oldest);
        break;
}
```

**Pattern intent:** Define a family of algorithms, encapsulate each one, and make them interchangeable.

**Why enum instead of polymorphic class?** There are only 3 strategies, each a few lines. Full polymorphism (virtual dispatch) would add unnecessary complexity and vtable overhead.

---

## 7. Strategy — AlertHandler Hierarchy

```cpp
class AlertHandler {
    virtual void onAlert(const Alert& alert) = 0;
};

class LoggingAlertHandler : public AlertHandler { ... };
class CallbackAlertHandler : public AlertHandler { ... };
class CompositeAlertHandler : public AlertHandler { ... };
class NullAlertHandler : public AlertHandler { ... };
```

**Strategy pattern:** The RealtimeProcessor holds an `AlertHandlerPtr` and calls `onAlert()` without knowing the concrete type.

**Composite pattern (also):** `CompositeAlertHandler` holds a vector of handlers and dispatches to all of them. This is the Gang-of-Four Composite — a tree structure where the composite and leaves share the same interface.

**Null Object pattern (also):** `NullAlertHandler` swallows all alerts. Used in testing to suppress alert noise.

---

## 8. Template Method — EventProcessor

```cpp
class EventProcessor {
public:
    virtual void process(const Event& event) = 0;  // template method
    virtual void start() = 0;
    virtual void stop() = 0;
};

// ProcessManager::runLoop() defines the skeleton:
while (isRunning_) {
    auto event = bus.pop(queueId, timeout);   // step 1: dequeue
    processor->process(*event);                // step 2: delegate to subclass
}
```

**Pattern intent:** Define the skeleton of an algorithm, letting subclasses override specific steps.

The `runLoop()` in ProcessManager is the invariant part (dequeue, error handling). Each processor's `process()` is the variant part.

---

## 9. RAII — Resource Acquisition Is Initialization

### Lock Guards

```cpp
{
    std::lock_guard<std::mutex> lock(storageMutex);
    storageFile.write(...);
}  // mutex automatically released
```

### Unique Pointers

```cpp
auto topicTable = std::make_shared<TopicTable>();
// Automatically deleted when all shared_ptr copies go out of scope
```

### Thread Joining in Destructors

```cpp
Dispatcher::~Dispatcher() noexcept {
    stop();  // sets running_ = false, joins worker_thread_
}
```

**Pattern intent:** Tie resource lifetime to object lifetime. Acquisition happens in the constructor, release in the destructor. This guarantees cleanup even if exceptions are thrown.

**The noexcept destructor** is critical — if a destructor throws during stack unwinding (another exception is active), `std::terminate()` is called. All destructors in the project are `noexcept`.

---

## 10. Pipeline — Architectural Pattern

```
Ingest → Dispatch → EventBus → Processor → Storage
```

Each stage:
1. Has a well-defined input and output.
2. Runs on its own thread(s).
3. Communicates via bounded queues.
4. Can be independently started/stopped.

**Pattern intent:** Decompose a complex process into a chain of independent stages.

**Benefits:**
- Each stage can be scaled independently (e.g., add more ingest threads).
- Bounded queues provide natural backpressure.
- Failures are isolated (a processor crash doesn't affect ingest).

---

## 11. Producer-Consumer — Concurrency Pattern

Three instances in the project:

| Queue | Producers | Consumer |
|-------|----------|----------|
| MPSC (inbound) | TCP/UDP ingest threads | Dispatcher thread |
| REALTIME (SPSC) | Dispatcher thread | RealtimeProcessor thread |
| TRANSACTIONAL/BATCH (deque) | Dispatcher thread | Respective processor thread |

**Pattern intent:** Decouple production rate from consumption rate via a buffer.

**Bounded buffer:** All queues have a capacity. When full, the producer either blocks (TRANSACTIONAL), drops the event (BATCH), or evicts the oldest (REALTIME). This prevents unbounded memory growth.

---

## 12. Backpressure — Reactive Pattern

```
Level 0: HEALTHY      → all flowing normally
Level 1: ELEVATED     → log warning, downgrade HIGH→MEDIUM
Level 2: DEGRADED     → drop batch events to DLQ
Level 3: CRITICAL     → pause transactional processor
Level 4: EMERGENCY    → pause + drop + DLQ everything
```

**Pattern intent:** When a consumer can't keep up, slow down the producer instead of buffering indefinitely.

**Cascading implementation:**
1. EventBus detects queue fill level → sets PressureLevel atomic.
2. Dispatcher reads PressureLevel → downgrades event priority.
3. Admin reads MetricSnapshots → ControlPlane decides action.
4. ControlPlane sets PipelineState → Dispatcher sleeps.
5. Processors respond to pause/drop commands.
6. TCP `recv()` blocks → kernel buffer fills → TCP window = 0 → end-to-end backpressure.

---

## 13. CRTP-like SFINAE in EventPool

```cpp
template<typename T>
static constexpr auto has_pool_index(int)
    -> decltype(std::declval<T>().pool_index_, std::true_type{}) { return {}; }

template<typename T>
static constexpr std::false_type has_pool_index(...) { return {}; }
```

This is not classical CRTP but uses the same compile-time polymorphism idea:
- At compile time, detect if `EventType` has `pool_index_`.
- Use `if constexpr` to select the fast path (direct index) or slow path (hash map lookup).
- **Zero runtime cost** — the decision is made during compilation.

---

## 14. Summary: When to Use Each Pattern

| Need | Pattern | Example |
|------|---------|---------|
| Exactly one global instance | Singleton | MetricRegistry |
| Reuse expensive objects | Object Pool | IngestEventPool |
| Encapsulate creation logic | Factory Method | EventFactory |
| Decouple notification | Observer | ProcessedEventStream |
| Interchangeable algorithms | Strategy | OverflowPolicy, AlertHandler |
| Skeleton algorithm with variants | Template Method | ProcessManager::runLoop |
| Treat group as individual | Composite | CompositeAlertHandler |
| Do-nothing placeholder | Null Object | NullAlertHandler |
| Automatic resource cleanup | RAII | lock_guard, unique_ptr, destructors |
| Staged processing | Pipeline | Ingest→Dispatch→Bus→Process→Store |
| Rate decoupling | Producer-Consumer | MPSC, SPSC, deque queues |
| Load shedding | Backpressure | 5-level cascade |

---

## 15. Deep Dive: SOLID Principles in EventStreamCore

### 15.1 Single Responsibility Principle (SRP)

> "A class should have only one reason to change."

| Class | Single Responsibility | Violation? |
|-------|----------------------|-----------|
| IngestServer | Accept network connections and read bytes | ✅ Clean |
| Dispatcher | Route events from MPSC to EventBus by topic | ✅ Clean |
| EventFactory | Create events with ID, CRC, timestamp | ✅ Clean |
| EventBus | Manage per-priority queues and overflow policies | ⚠️ Borderline — also handles backpressure signaling |
| AdminController | Monitor metrics AND decide control actions | ⚠️ Could split into MetricMonitor + ControlDecider |
| ProcessManager | Create processors, manage threads, run event loops | ⚠️ Two concerns: thread management + processor lifecycle |

**Interview insight:** Real-world code often has borderline SRP violations. The key is recognizing them and articulating the trade-off: splitting AdminController into two classes adds indirection and inter-class communication overhead for no functional benefit.

### 15.2 Open/Closed Principle (OCP)

> "Open for extension, closed for modification."

**Exemplary:** `AlertHandler` hierarchy — adding a new alert destination (email, Slack) requires only adding a new subclass. Zero changes to `RealtimeProcessor` or existing handlers.

**Exemplary:** `OverflowPolicy` enum — adding a new policy requires adding an enum value and a `case` in the switch. However, this IS a modification (OCP violation). True OCP would use polymorphic strategy objects. The project chose simplicity over purity.

### 15.3 Liskov Substitution Principle (LSP)

> "Subtypes must be substitutable for their base types."

**Test:** Can you replace `LoggingAlertHandler` with `NullAlertHandler` in `RealtimeProcessor` without changing behavior? Yes — the processor calls `onAlert()` and doesn't depend on the side effects (logging vs. silence).

**Test:** Can you replace `RealtimeProcessor` with `TransactionalProcessor`? No — they have different `process()` semantics (dedup, retries). They don't share a base class that defines substitutability. This is correct — LSP doesn't require all classes to be interchangeable, only those that share a base type.

### 15.4 Interface Segregation Principle (ISP)

> "No client should be forced to depend on methods it doesn't use."

**Good:** `ProcessedEventObserver` has only 2 methods: `onEventProcessed()` and `onEventDropped()`. An observer that only cares about processed events still must implement `onEventDropped()` (as no-op). A stricter ISP design would split into two interfaces.

**Good:** `AlertHandler` has exactly 1 method: `onAlert()`. This is the ideal interface — impossible to violate ISP.

### 15.5 Dependency Inversion Principle (DIP)

> "Depend on abstractions, not concretions."

**Exemplary:** `ProcessManager` depends on `EventProcessor*` (abstract), not on `RealtimeProcessor` directly.

**Violation:** `main.cpp` constructs concrete types directly:
```cpp
auto realtimeProc = std::make_unique<RealtimeProcessor>(...);
```
This is acceptable — `main()` is the composition root where concrete types are wired together. DIP doesn't apply at the composition root.

---

## 16. Deep Dive: Anti-Pattern Catalog

### What the project avoids (and why)

| Anti-Pattern | Description | How Avoided |
|-------------|-------------|-------------|
| **God Object** | One class does everything | Responsibility split across 15+ classes |
| **Spaghetti Code** | Tangled control flow | Clean pipeline: Ingest→Dispatch→Bus→Process→Store |
| **Premature Optimization** | Optimizing before profiling | Lock-free only on measured hot paths (MPSC, SPSC) |
| **Lava Flow** | Dead code left "just in case" | Distributed/Raft layer was removed when no longer needed |
| **Golden Hammer** | Using one pattern everywhere | Uses locks where simple (TopicTable), lock-free where critical (MPSC) |
| **Busy-Wait** | Spinning consuming CPU | `sleep_for(100ms)` and `condition_variable::wait_for()` for idle threads |
| **Shared Mutable State** | Global variables without synchronization | Every shared variable is either atomic or mutex-protected |
| **Magic Numbers** | Unexplained literal values | Constants like `SPSC_CAPACITY = 16384`, `DEDUP_BUCKETS = 4096` |

### What the project could improve

| Potential Anti-Pattern | Current State | Improvement |
|----------------------|---------------|-------------|
| **Hardcoded configuration** | Some values are compile-time constants | Make all configurable via YAML |
| **Error code ambiguity** | Bridge returns -1 for all errors | Return distinct codes (-1 = null ptr, -2 = not initialized, -3 = queue full) |
| **Test coverage gaps** | Unit tests exist but no integration tests | Add end-to-end pipeline test |

---

## 17. Deep Dive: Gang-of-Four (GoF) Comparison

### Patterns used vs. GoF catalog

| GoF Category | GoF Pattern | Used? | Reason |
|-------------|-------------|-------|--------|
| **Creational** | Abstract Factory | ❌ | Only one Event type family |
| | Builder | ❌ | Events are simple enough for factory |
| | Factory Method | ✅ | EventFactory |
| | Prototype | ❌ | No need for clone-based creation |
| | Singleton | ✅ | MetricRegistry, ProcessedEventStream |
| **Structural** | Adapter | ❌ | No legacy interface wrapping |
| | Bridge | ❌ | (confusingly, the C "bridge" is FFI, not GoF Bridge) |
| | Composite | ✅ | CompositeAlertHandler |
| | Decorator | ❌ | Could be used for processor chaining |
| | Facade | ✅ | C API bridge is a Facade over the engine |
| | Flyweight | ❌ | Event pool is similar but not GoF Flyweight |
| | Proxy | ❌ | No remote/lazy/protection proxy needed |
| **Behavioral** | Chain of Responsibility | ❌ | Pipeline stages aren't "pass-or-handle" — all process |
| | Command | ❌ | ControlAction is similar but simpler |
| | Iterator | ❌ | Standard containers handle iteration |
| | Mediator | ✅ | EventBus mediates between Dispatcher and Processors |
| | Memento | ❌ | No undo/restore needed |
| | Observer | ✅ | ProcessedEventStream |
| | State | ⚠️ | PipelineState enum with behavior change — lightweight State |
| | Strategy | ✅ | OverflowPolicy, AlertHandler |
| | Template Method | ✅ | ProcessManager::runLoop |
| | Visitor | ❌ | No double-dispatch needed |

**Interview insight:** Not using a pattern is as important as using one. The project doesn't use Decorator for processor chaining because the pipeline is linear and fixed — adding decorators would add complexity with no benefit.

---

## 18. Deep Dive: When NOT to Use Each Pattern

| Pattern | When NOT to Use | Risk if Misapplied |
|---------|----------------|-------------------|
| Singleton | When testability matters more than convenience | Hard to mock, hidden dependencies, prevents parallel testing |
| Object Pool | When objects are cheap to construct | Pool management overhead exceeds allocation cost |
| Observer | When notification order matters | Observers execute in arbitrary order → non-deterministic behavior |
| Strategy (polymorphic) | When there are only 2-3 strategies | Virtual dispatch overhead for no flexibility gain |
| Template Method | When steps vary dramatically between subclasses | Base class becomes a "Swiss Army knife" with too many hooks |
| RAII | When ownership is unclear or shared | Use `shared_ptr` instead of raw RAII wrapper |
| Pipeline | When stages have different throughput requirements | Slowest stage becomes bottleneck → need per-stage scaling |

---

## 19. Deep Dive: Dependency Injection Without a Framework

The project uses **constructor injection** — dependencies are passed as parameters:

```cpp
// ProcessManager constructor
ProcessManager(EventBus& bus,
               StorageEngine& storage,
               DLQManager& dlq,
               AdminController& admin);
```

**Advantages over service locator (global static access):**
- Clear: reading the constructor signature tells you all dependencies.
- Testable: pass mock implementations in tests.
- No hidden coupling.

**The composition root (`main.cpp`):**
```cpp
auto storage = StorageEngine("data/");
auto dlq = DLQManager(1000);
auto bus = EventBus();
auto admin = AdminController(bus, storage);
auto manager = ProcessManager(bus, storage, dlq, admin);
```

All wiring happens in one place. No other file needs to know about concrete types.

---

## 20. Extended Interview Questions

**Q1: The project uses `enum class` for OverflowPolicy instead of polymorphic Strategy. When would you switch to polymorphism?**  
A: When the number of strategies grows beyond 5-6, or when each strategy has complex state (e.g., a rate-limiting strategy that tracks a token bucket). At that point, the switch statement becomes unwieldy and violates OCP.

**Q2: Is MetricRegistry's Singleton pattern testable?**  
A: Barely. You can't inject a mock registry. To improve testability: (a) make `getInstance()` return a reference to a base class `IMetricRegistry`, (b) add a `setInstance()` for tests, or (c) pass the registry as a constructor parameter everywhere (dependency injection). The project chose simplicity over testability.

**Q3: The Observer pattern copies the observer vector before notification. What's the performance cost?**  
A: `std::vector` copy is O(N) where N is the number of observers. With 5 observers, this is ~5 pointer copies (40 bytes) — negligible. With 10,000 observers, it's a 80KB copy per event — significant. The project expects ≤10 observers.

**Q4: Name a design pattern the project does NOT use but should consider.**  
A: **Circuit Breaker** — for the bridge and SDK layer. If the C++ engine is consistently failing (e.g., storage full), the Python SDK should "open the circuit" and fail fast instead of queuing events that will fail. After a timeout, try one event ("half-open"). If it succeeds, close the circuit and resume normal flow.

**Q5: How does the project avoid the Service Locator anti-pattern?**  
A: Service Locator hides dependencies behind a global registry (`ServiceLocator::get<IStorage>()`). The project uses constructor injection — all dependencies are visible in the constructor signature. The two exceptions (Singleton MetricRegistry and ProcessedEventStream) ARE service locators, but they're acceptable because metrics and event observation are cross-cutting concerns, not business logic dependencies.
