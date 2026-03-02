# 03 — Memory Management & NUMA

> **Goal:** Understand every memory optimisation in EventStreamCore — object pools, cache-line alignment, NUMA topology, and memory-policy binding.

---

## 1. Why Memory Matters in Event Streaming

Every event in the hot path allocates:
- An `Event` object (~200 bytes: header + string + vector + map)
- A `shared_ptr` control block (16 bytes)
- Possibly a `Node` in the MPSC queue (24+ bytes)

At 1 million events/second, that's **~240 MB/s of allocations**. Standard `malloc` uses a global free-list protected by a lock → contention. The project uses three strategies to fight this:

1. **Object pooling** — pre-allocate, reuse, zero-alloc in steady state.
2. **Cache-line alignment** — prevent false sharing between threads.
3. **NUMA binding** — keep thread and data on the same memory controller.

---

## 2. IngestEventPool — Shared-Pointer Object Pool

**File:** `include/eventstream/core/ingest/ingest_pool.hpp`

### 2.1 How It Works

```
initialize():
  ┌─────────────────────────────┐
  │  std::queue<unique_ptr<Event>> │  10 000 pre-allocated Event objects
  └─────────────────────────────┘

acquireEvent():
  pool.pop() → unique_ptr<Event>
  convert to shared_ptr<Event> with CUSTOM DELETER
  return shared_ptr

When shared_ptr refcount → 0:
  custom deleter fires:
    *e = Event{};           // reset (move-assign default)
    pool.push(unique_ptr(e))  // return to pool
```

### 2.2 The Custom Deleter (Key Concept)

```cpp
static std::shared_ptr<Event> acquireEvent() {
    // ... acquire unique_ptr from pool ...
    return std::shared_ptr<Event>(evt.release(), [](Event* e) {
        if (!e) return;
        if (getShutdownFlag().load(std::memory_order_acquire)) {
            delete e;   // during shutdown, just free
            return;
        }
        *e = Event{};   // reset fields (move-assignment)
        {
            std::lock_guard<std::mutex> lock(getPoolMutex());
            if (getShutdownFlag().load(std::memory_order_acquire)) {
                delete e;   // double-check under lock
                return;
            }
            auto& pool = getPool();
            if (pool.size() < kPoolCapacity) {
                pool.push(std::unique_ptr<Event>(e));
            } else {
                delete e;   // pool is full, discard
            }
        }
    });
}
```

**Why `*e = Event{}` instead of placement new?**  
Placement new (`e->~Event(); new (e) Event();`) is fragile — if the destructor throws or the type has non-trivial alignment, it's undefined behavior. Move-assignment is safe, standard, and the compiler optimizes it to the same code.

### 2.3 Thread Safety

The pool itself is mutex-protected (`getPoolMutex()`). This is acceptable because:
- `acquireEvent()` is called once per incoming network message (not per field access).
- The lock is held for a single `queue::push`/`pop` — microseconds.
- Under extreme load, if the pool is exhausted, it falls back to `new Event()` (heap allocation).

### 2.4 Shutdown Safety

The `getShutdownFlag()` is an `atomic<bool>` checked **both outside and inside** the lock:

1. **Outside lock (fast path):** If shutdown is in progress, delete immediately.
2. **Inside lock (slow path):** Double-check after acquiring the lock, because another thread might have called `shutdown()` between the first check and the lock acquisition.

This is a variant of the **double-checked locking pattern**.

---

## 3. EventPool — Template Object Pool with SFINAE

**File:** `include/eventstream/core/memory/event_pool.hpp`

### 3.1 Design

A compile-time polymorphic pool that adapts to the event type:

```cpp
template<typename EventType, size_t Capacity>
class EventPool {
    std::array<std::unique_ptr<EventType>, Capacity> pool_;
    std::array<size_t, Capacity> free_list_;   // stack of free indices
    size_t available_count_;
```

### 3.2 SFINAE: Detecting `pool_index_`

The pool needs to know if the event type has a `pool_index_` member for O(1) release:

```cpp
template<typename T>
static constexpr auto has_pool_index(int) 
    -> decltype(std::declval<T>().pool_index_, std::true_type{}) {
    return {};
}

template<typename T>
static constexpr std::false_type has_pool_index(...) {
    return {};
}

static constexpr bool kHasPoolIndex = decltype(has_pool_index<EventType>(0))::value;
```

**How this works:**
1. The first overload tries `std::declval<T>().pool_index_`. If the expression is valid (the type *has* the member), this overload is selected → returns `true_type`.
2. If it fails (SFINAE — Substitution Failure Is Not An Error), the second overload (ellipsis `...`) is selected → returns `false_type`.
3. `kHasPoolIndex` is a compile-time boolean.

### 3.3 `if constexpr` — Compile-Time Branching

```cpp
void release(EventType* obj) {
    size_t idx = SIZE_MAX;
    if constexpr (kHasPoolIndex) {
        idx = obj->pool_index_;   // O(1): read the embedded index
    } else {
        auto it = ptr_to_index_.find(obj);  // O(1) amortized: hash map lookup
        if (it == ptr_to_index_.end()) { delete obj; return; }
        idx = it->second;
    }
    // ... return to free list ...
}
```

- `if constexpr` is a **C++17 feature** — the dead branch is not compiled at all.
- `HighPerformanceEvent` has `pool_index_` → uses the fast path.
- Regular `Event` (from `events/event.hpp`) doesn't → uses the `unordered_map` fallback.

### 3.4 Free-List (Stack-Based)

```
free_list_:  [0, 1, 2, 3, 4, 5, 6, 7]    available_count_ = 8
              ↑ next to give out

acquire():   available_count_--
             idx = free_list_[7]  → returns pool_[7]

release(ptr): free_list_[available_count_] = idx
              available_count_++
```

This is O(1) acquire and O(1) release — no searching.

---

## 4. HighPerformanceEvent — Cache-Optimized Event

**File:** `include/eventstream/core/events/event_hp.hpp`

### 4.1 Layout

```cpp
struct alignas(64) HighPerformanceEvent {
    size_t pool_index_{SIZE_MAX};          // for EventPool
    uint64_t event_id;
    uint32_t topic_id;
    uint64_t ingest_timestamp_ns;
    uint64_t dequeue_timestamp_ns;
    uint64_t process_done_timestamp_ns;
    uint32_t payload_size;
    uint32_t source_type;
    static constexpr size_t PAYLOAD_SIZE = 512;
    uint8_t payload[PAYLOAD_SIZE];
};
```

**Design decisions:**

| Decision | Reason |
|----------|--------|
| `alignas(64)` | Starts on a cache-line boundary. Prevents false sharing when multiple events sit in an array. |
| Fixed 512-byte payload | No heap allocation for body. Entire event is one contiguous memory block → cache-prefetch friendly. |
| No `std::string`, no `std::vector` | Avoids pointer indirection. The CPU can read the entire event without chasing pointers. |
| `topic_id` (uint32) instead of `topic` (string) | Integer comparison is 1 instruction; string comparison requires a loop. |
| Embedded timestamps | Latency measured without extra allocation: `total_latency_ns() = process_done - ingest`. |

### 4.2 Size Verification

```cpp
static_assert(alignof(HighPerformanceEvent) == 64);
static_assert(sizeof(HighPerformanceEvent) >= 576);
// 576 bytes = 9 cache lines
```

Nine cache lines per event. When the SPSC ring buffer reads sequentially, the hardware prefetcher loads the next event automatically.

---

## 5. NUMA Binding

**File:** `include/eventstream/core/memory/numa_binding.hpp`

### 5.1 What Is NUMA?

**Non-Uniform Memory Access** — in multi-socket servers, each CPU socket has its own memory controller:

```
        ┌─────────────┐     ┌─────────────┐
        │   Socket 0   │     │   Socket 1   │
        │  CPU 0-7     │     │  CPU 8-15    │
        │  DDR4 64GB   │────►│  DDR4 64GB   │
        │  (local RAM) │◄────│  (local RAM) │
        └─────────────┘     └─────────────┘
                ↕ QPI interconnect ↕
```

Accessing **local memory** (same socket) takes ~80ns. Accessing **remote memory** (cross-socket via QPI) takes ~140ns — **75% slower**.

### 5.2 Thread-to-CPU Binding

```cpp
static bool bindThreadToCPU(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}
```

**Why bind?** The OS scheduler can migrate threads between cores. If a thread moves from CPU 0 (Socket 0) to CPU 8 (Socket 1), all its data in Socket 0's cache becomes remote. Binding prevents this.

### 5.3 NUMA-Aware Memory Allocation

```cpp
static void* allocateOnNode(size_t size, int numa_node) {
    return numa_alloc_onnode(size, numa_node);
}
```

`numa_alloc_onnode()` calls the kernel to allocate pages from a specific node's physical memory. This guarantees the memory is local to the threads running on that node.

### 5.4 Memory Policy Binding

```cpp
static bool setMemoryPolicy(void* ptr, size_t size, int numa_node) {
    struct bitmask* nodeset = numa_bitmask_alloc(getNumNumaNodes());
    numa_bitmask_setbit(nodeset, numa_node);
    int ret = mbind(ptr, size, MPOL_BIND, nodeset->maskp, nodeset->size + 1, 0);
    numa_bitmask_free(nodeset);
    return ret == 0;
}
```

`mbind()` is a Linux system call that sets the **memory policy** for a range of virtual addresses. `MPOL_BIND` says "only allocate pages from these specific NUMA nodes." If the node's memory is full, allocation fails (rather than falling back to remote memory).

### 5.5 Usage in the Project

```cpp
// ProcessManager::start() — pin RealtimeProcessor to core 2
realtimeThread_ = std::thread(&ProcessManager::runLoop, this,
    EventBusMulti::QueueId::REALTIME, realtimeProcessor_.get());
NUMABinding::bindThread(realtimeThread_, 2);

// Inside each processor::process() — lazy binding
static thread_local bool bound = false;
if (!bound && numa_node_ >= 0) {
    NUMABinding::bindThreadToNUMANode(numa_node_);
    bound = true;
}
```

The `thread_local` flag ensures binding happens **exactly once per thread** — not on every event.

### 5.6 Topology Printing

```cpp
NUMABinding::printTopology();
// Output:
// [NUMA] ════════════════════════════════════
// [NUMA] NUMA Topology (nodes=2)
// [NUMA] Node 0: 65536MB, CPUs: 0,1,2,3,4,5,6,7
// [NUMA] Node 1: 65536MB, CPUs: 8,9,10,11,12,13,14,15
// [NUMA] ════════════════════════════════════
```

---

## 6. Cache-Line Alignment Throughout the Project

| Structure | Field(s) | Why |
|-----------|----------|-----|
| `SpscRingBuffer` | `buffer_`, `head_`, `tail_` | Producer/consumer on different cores |
| `MpscQueue` | `head_`, `tail_`, `size_` | Multiple producers + single consumer |
| `HighPerformanceEvent` | Entire struct | Array of events → no cross-event false sharing |
| `BatchProcessor::TopicBucket` | `events`, `bucket_mutex` | Separate buckets don't interfere |

---

## 7. Interview Deep-Dive Questions

**Q1: What is false sharing? Give an example from this project.**  
A: When two threads access different variables that happen to be on the same 64-byte cache line, each write invalidates the other core's cache copy. Example: without `alignas(64)` on `head_` and `tail_` in `SpscRingBuffer`, the producer updating `head_` would invalidate the consumer's cache line containing `tail_`.

**Q2: What is the difference between `numa_alloc_onnode()` and `malloc()`?**  
A: `malloc()` uses the default first-touch policy — the page is allocated on the node of the CPU that first writes to it. `numa_alloc_onnode()` explicitly requests pages from a specific NUMA node regardless of which CPU does the first touch.

**Q3: Why does the IngestEventPool use `std::queue<unique_ptr>` instead of `std::vector`?**  
A: `std::queue` gives FIFO reuse — the most recently returned event is the last to be reused, keeping it "warm" in cache. `std::vector` would require tracking a free index.

**Q4: Explain the double-checked locking in `IngestEventPool::acquireEvent()`'s deleter.**  
A: First check (no lock): fast-path skip during shutdown. Second check (under lock): races with `shutdown()` between first check and lock acquisition. Without the second check, we could push an event back into a pool that's being destroyed.

**Q5: What does `if constexpr` buy you that a regular `if` doesn't?**  
A: `if constexpr` eliminates the dead branch at compile time. With a regular `if`, both branches must compile — if `EventType` lacks `pool_index_`, accessing it would be a compile error even inside an `if (false)` branch.

---

## 8. Deep Dive: How `malloc` Works and Why Pools Beat It

### 8.1 glibc `malloc` Internals (ptmalloc2)

```
Thread calls malloc(200):
  1. Check thread-local arena (per-thread free list) → O(1) if cache hit
  2. Search arena's bins (small, large, unsorted) → O(log n)
  3. If no fit: extend heap via brk()/mmap() → kernel syscall (~1μs)
  4. Return pointer

Thread calls free(ptr):
  1. Determine arena from chunk metadata
  2. Coalesce with adjacent free chunks
  3. Add to arena's free list
  4. If large enough: munmap() → return pages to kernel
```

**Problems at 1M events/sec:**
- **Lock contention:** Each arena has a mutex. Under heavy load, threads contend.
- **Fragmentation:** Repeated malloc/free of different sizes → external fragmentation → more `mmap` calls.
- **TLB pressure:** `mmap` creates new virtual memory mappings → TLB misses (~10ns each).
- **Cache pollution:** Newly mapped pages are cold (not in L1/L2).

### 8.2 Why IngestEventPool Wins

| Metric | malloc/free | IngestEventPool |
|--------|------------|-----------------|
| Allocation time | 50-500 ns | ~50 ns (mutex + queue pop) |
| Deallocation time | 30-300 ns | ~50 ns (mutex + queue push) |
| Syscalls | Occasional mmap/brk | Zero (pre-allocated) |
| Fragmentation | Grows over time | Zero (fixed-size objects) |
| Cache behavior | Cold pages | Warm (recently used objects) |
| Memory overhead | 16 bytes/chunk metadata | Zero (pool manages externally) |

### 8.3 Alternative: jemalloc/tcmalloc

Modern allocators (jemalloc, tcmalloc) use per-thread caches and size-class binning:
- **Per-thread cache:** Each thread has a local free list → no lock for small allocations.
- **Size classes:** Objects rounded up to nearest size class (e.g., 200 → 256 bytes) → no fragmentation within a class.

Even with jemalloc, a dedicated pool is faster because:
1. No size-class rounding (exact-fit).
2. No metadata per object.
3. Objects are reused in FIFO order → maximizes cache warmth.

---

## 9. Deep Dive: Virtual Memory, Page Tables, and TLB

### 9.1 Address Translation

```
Virtual Address (48-bit on x86-64):
┌──────┬──────┬──────┬──────┬──────────┐
│ PML4 │ PDPT │  PD  │  PT  │  Offset  │
│ 9bit │ 9bit │ 9bit │ 9bit │  12bit   │
└──────┴──────┴──────┴──────┴──────────┘

Each level: 512 entries × 8 bytes = 4 KB page

Total page walk: 4 memory accesses → ~200 cycles without TLB
TLB hit: 1 cycle
```

### 9.2 TLB Pressure from Pool vs malloc

**IngestEventPool:** 10,000 events × 200 bytes = 2 MB → fits in ~500 pages → ~500 TLB entries → fits in L2 TLB (typically 1024-2048 entries). **Zero TLB misses** in steady state.

**malloc scattered allocations:** Events spread across many pages due to interleaving with other allocations. 10,000 events might touch 2,000+ pages → exceeds TLB → frequent page walks.

### 9.3 Huge Pages (2MB)

For even better TLB efficiency, NUMA allocation can use huge pages:

```cpp
// mmap with MAP_HUGETLB:
void* ptr = mmap(nullptr, size, PROT_READ|PROT_WRITE,
                 MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
```

One 2MB huge page covers the entire pool → **1 TLB entry** instead of 500. The project doesn't use huge pages explicitly, but `numa_alloc_onnode` can be configured to use the huge page pool.

---

## 10. Deep Dive: Cache Hierarchy and Event Layout

### 10.1 Cache Hierarchy (Typical Server CPU)

```
Core 0:
  L1d: 32 KB, 4-cycle latency, 8-way associative
  L1i: 32 KB (instruction cache)
  L2:  256 KB, 12-cycle latency, 8-way associative
  
Shared across cores:
  L3:  30 MB, 40-cycle latency, 16-way associative
  
Memory:
  DRAM: ~100-cycle latency (local NUMA), ~300-cycle (remote)
```

### 10.2 HighPerformanceEvent: Cache-Optimal Layout

```cpp
struct alignas(64) HighPerformanceEvent {  // 576 bytes = 9 cache lines
    size_t   pool_index_;               // Line 0: management
    uint64_t event_id;                  // Line 0: hot identification
    uint32_t topic_id;                  // Line 0: hot routing
    uint64_t ingest_timestamp_ns;       // Line 0-1: hot timing
    uint64_t dequeue_timestamp_ns;      // Line 1: hot timing
    uint64_t process_done_timestamp_ns; // Line 1: cold (set late)
    uint32_t payload_size;              // Line 1: hot metadata
    uint32_t source_type;               // Line 1: cold metadata
    uint8_t  payload[512];              // Lines 1-9: payload
};
```

**Why inline payload?** A `std::vector<uint8_t>` stores data on the heap:
```
Event on stack: [header | vector{ptr, size, cap}]  → pointer chase → heap
HighPerformanceEvent: [header | payload_inline]     → contiguous, prefetchable
```

The CPU prefetcher detects sequential access patterns. With inline payload, accessing `event_id` automatically prefetches the payload into L1. With vector, the pointer indirection defeats the prefetcher.

### 10.3 Event vs HighPerformanceEvent: When to Use Which

| | `Event` | `HighPerformanceEvent` |
|---|---------|----------------------|
| Payload size | Unlimited (heap vector) | ≤ 512 bytes |
| Topic | `std::string` (heap) | `uint32_t topic_id` (requires mapping) |
| Metadata | `unordered_map` | None |
| Cache misses per access | 3+ (vector ptr, string ptr, map ptr) | 0 (everything inline) |
| Use case | General pipeline | SPSC hot path, benchmarks |
| Size | ~200 bytes + heap | 576 bytes (fixed) |

---

## 11. Deep Dive: shared_ptr Control Block Layout

```
std::shared_ptr<Event>:
  ┌──────────────────┐
  │ Event* ptr       │ → points to managed object
  │ ControlBlock* cb │ → points to control block
  └──────────────────┘

Control Block (16 bytes on x86-64):
  ┌──────────────────┐
  │ strong_count: 2  │ atomic<long> — number of shared_ptr copies
  │ weak_count: 1    │ atomic<long> — number of weak_ptr copies + 1
  │ deleter (opt)    │ stored if custom deleter provided
  │ allocator (opt)  │ stored if custom allocator provided
  └──────────────────┘
```

**Every shared_ptr copy:** `strong_count.fetch_add(1, memory_order_relaxed)` → `lock xadd` on x86. ~20 cycles.

**Every shared_ptr destroy:** `strong_count.fetch_sub(1, memory_order_acq_rel)` → `lock xadd` with acquire/release. ~25 cycles. If count reaches 0, calls deleter.

**Why this matters:** In the pipeline, an event may be copied 3-5 times (ingest → MPSC → dispatcher → EventBus → processor). That's 6-10 atomic increments/decrements per event. At 1M events/sec → 6-10M atomic ops/sec just for reference counting.

**Optimization in the project:** Events are `std::move`'d wherever possible. Moving a `shared_ptr` is free (just pointer swap, no atomic op).

---

## 12. Deep Dive: The `*e = Event{}` Reset Pattern

```cpp
// In IngestEventPool custom deleter:
*e = Event{};
```

This is a **move-assignment from a default-constructed temporary**:

1. `Event{}` constructs a temporary with default values (empty string, empty vector, empty map).
2. `*e = Event{}` invokes `Event::operator=(Event&&)` — the move-assignment operator.
3. The old `e->topic` string's heap memory is swapped into the temporary → freed when temporary destructs.
4. The old `e->body` vector's heap memory is similarly freed.
5. `e` is now a clean, empty Event — ready for reuse.

**Why not `memset(e, 0, sizeof(Event))`?** `Event` has non-trivial members (`std::string`, `std::vector`, `std::unordered_map`). `memset` would corrupt their internal state (zero the size/capacity without freeing the heap memory → memory leak + UB on next use).

---

## 13. Extended Interview Questions

**Q6: What happens if two threads call `IngestEventPool::acquireEvent()` simultaneously?**  
A: Both enter `std::lock_guard<std::mutex> lock(getPoolMutex())`. One wins; the other blocks on the mutex. The winner pops from the queue and releases the lock. The loser then enters and pops the next event. The pool is thread-safe but serialized — at most one thread can acquire/release at a time. This ~50ns overhead is acceptable because the pool is not on the hottest path (event processing is far slower than acquisition).

**Q7: Explain the difference between `numa_alloc_onnode()` and first-touch policy.**  
A: First-touch (default Linux policy): the kernel allocates the physical page on the NUMA node of the CPU that first writes to the virtual address. This means `malloc` + first write from core 0 → page on node 0. `numa_alloc_onnode(size, 1)` explicitly binds the allocation to node 1 regardless of which CPU calls it. Use `numa_alloc_onnode` when the allocating thread is different from the consuming thread (common in initialization code).

**Q8: Why does `HighPerformanceEvent` use `uint32_t topic_id` instead of `std::string topic`?**  
A: `std::string` has 32 bytes overhead (pointer + size + capacity + SSO buffer) and may allocate heap memory for topics > 15 characters. `uint32_t` is 4 bytes, inline, and can be used as a direct array index for O(1) topic lookup. The mapping from string to ID happens once at ingestion time; all subsequent routing uses the integer ID.

**Q9: What is the "cold start" problem with object pools?**  
A: When the pool is first initialized, all 10,000 events are allocated but never used. Their memory pages may not be mapped yet (demand paging) or may be on a remote NUMA node (first-touch hasn't occurred). First access to each event triggers a page fault (~1000ns). Mitigation: after `initialize()`, touch each event: `for (auto& e : pool) memset(e.get(), 0, sizeof(Event));` This forces page mapping on the initializing thread's NUMA node.

**Q10: How much memory does the IngestEventPool actually use?**  
A: 10,000 × sizeof(Event) + 10,000 × sizeof(unique_ptr) + queue overhead.
- `sizeof(Event)` ≈ 200 bytes (header 48 + string 32 + vector 24 + map 56 + padding)
- `sizeof(unique_ptr<Event>)` = 8 bytes
- Queue overhead: ~10,000 × 8 bytes (pointer per queue node in std::queue<deque>)
- Total: ~200 × 10,000 + 8 × 10,000 + 80,000 ≈ **2.16 MB**
- Plus heap allocations for each Event's string/vector/map: depends on usage (0 when reset).
