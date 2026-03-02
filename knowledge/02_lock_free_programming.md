# 02 — Lock-Free Programming

> **Goal:** Master every lock-free data structure in EventStreamCore — understand the memory-ordering guarantees, why each `std::memory_order` is chosen, and what would break if you changed it.

---

## 1. Why Lock-Free?

In a high-throughput event pipeline, contention on `std::mutex` creates two problems:

1. **Priority inversion** — a low-priority thread holding the lock blocks a high-priority one.
2. **Cache-line bouncing** — `mutex` state lives on a single cache line; every `lock()`/`unlock()` forces a cache-line transfer between CPU cores.

Lock-free structures eliminate both issues by relying on **atomic compare-and-swap (CAS)** instructions that the CPU executes in a single bus transaction.

---

## 2. SPSC Ring Buffer (`SpscRingBuffer`)

**Files:** `include/eventstream/core/queues/spsc_ring_buffer.hpp`, `src/core/queues/spsc_ring_buffer.cpp`

### 2.1 Design

```
┌───┬───┬───┬───┬───┬───┬───┬───┐
│   │   │ T │ x │ x │ x │ H │   │  Capacity = 8 (power of 2)
└───┴───┴───┴───┴───┴───┴───┴───┘
          ↑ tail          ↑ head
          consumer        producer
```

- **Single Producer** writes at `head`, advances it.
- **Single Consumer** reads at `tail`, advances it.
- **No lock needed** because head is only written by producer, tail only by consumer.

### 2.2 Power-of-2 Trick

```cpp
static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

size_t next = (head + 1) & (Capacity - 1);  // instead of (head + 1) % Capacity
```

Bitwise AND replaces expensive modulo division. For Capacity = 16384:

```
16384 - 1 = 0x3FFF = 0011 1111 1111 1111
(head + 1) & 0x3FFF  →  wraps around to 0 when head reaches 16384
```

### 2.3 Memory Ordering — Line by Line

```cpp
// PUSH (producer)
bool push(const T& item) {
    size_t head = head_.load(std::memory_order_relaxed);       // ①
    size_t next = (head + 1) & (Capacity - 1);
    if (next == tail_.load(std::memory_order_acquire)) {       // ②
        return false;  // queue is full
    }
    buffer_[head] = item;                                      // ③
    head_.store(next, std::memory_order_release);              // ④
    return true;
}
```

| Line | Order | Why |
|------|-------|-----|
| ① | `relaxed` | Only the producer writes `head_`; it always reads its own latest value. No ordering needed relative to other threads. |
| ② | `acquire` | Must see the consumer's latest `tail_` write. `acquire` ensures all writes the consumer did *before* updating `tail_` are visible. |
| ③ | (plain write) | Writes the item into the buffer. |
| ④ | `release` | Publishes both the buffer write ③ *and* the new head. The consumer's `acquire` load of `head_` will see the item. |

```cpp
// POP (consumer)
std::optional<T> pop() {
    size_t tail = tail_.load(std::memory_order_relaxed);       // ⑤
    if (tail == head_.load(std::memory_order_acquire)) {       // ⑥
        return std::nullopt;  // queue is empty
    }
    T item = buffer_[tail];                                    // ⑦
    tail_.store((tail + 1) & (Capacity - 1),
                std::memory_order_release);                    // ⑧
    return item;
}
```

| Line | Order | Why |
|------|-------|-----|
| ⑤ | `relaxed` | Only the consumer writes `tail_`. |
| ⑥ | `acquire` | Must see the producer's latest `head_` and the buffer write that preceded it. |
| ⑦ | (plain read) | Reads the item; safe because ⑥ synchronized with ④. |
| ⑧ | `release` | Tells the producer "I've consumed up to here; you can reuse this slot." |

### 2.4 False-Sharing Prevention

```cpp
alignas(64) T buffer_[Capacity];
alignas(64) std::atomic<size_t> head_{0};
alignas(64) std::atomic<size_t> tail_{0};
```

Each field sits on its **own 64-byte cache line**. Without this, `head_` and `tail_` could share a cache line, causing the producer and consumer cores to fight over ownership even though they access *different* variables.

### 2.5 Interview Questions

**Q: What breaks if you use `memory_order_relaxed` everywhere?**  
A: The consumer could read `head_` before the producer's buffer write is visible → garbage data.

**Q: Could you use `seq_cst` instead?**  
A: Correct but slower. `seq_cst` adds a full memory fence (`mfence` on x86); `acquire/release` is sufficient for a single-producer/single-consumer pattern because there is no third thread observing both atomics.

---

## 3. Vyukov MPSC Queue (`MpscQueue`)

**File:** `include/eventstream/core/queues/mpsc_queue.hpp`

### 3.1 Design

Dmitry Vyukov's MPSC queue uses a **dummy/sentinel node** so the consumer never touches the tail:

```
  tail ──────────►┌──────┐    ┌──────┐    ┌──────┐
                  │ node3│───►│ node2│───►│ node1│───► [dummy/head]
                  └──────┘    └──────┘    └──────┘
  (producers append here)                 (consumer reads here)
```

### 3.2 Push (Multiple Producers)

```cpp
bool push(const T& item) {
    if (size_.load(std::memory_order_relaxed) >= Capacity) {
        return false;                                           // ①
    }
    Node* node = new Node(item);
    Node* prev = tail_.exchange(node, std::memory_order_acq_rel); // ②
    prev->next.store(node, std::memory_order_release);            // ③
    size_.fetch_add(1, std::memory_order_relaxed);
    return true;
}
```

| Line | What happens | Why this ordering |
|------|-------------|-------------------|
| ① | Capacity check | `relaxed` — approximate is fine; worst case we allow one extra push. |
| ② | `exchange` atomically swaps `tail_` to point to the new node and returns the *previous* tail. `acq_rel` because this both reads (acquires) and writes (releases) the tail pointer. |
| ③ | Links the previous tail's `next` to the new node. `release` ensures the new node's data is visible before the consumer follows the `next` pointer. |

**Key insight:** Step ② is a single atomic RMW. Even if 10 threads call `push()` simultaneously, `exchange` serializes them — each gets a unique `prev`.

### 3.3 Pop (Single Consumer)

```cpp
std::optional<T> pop() {
    Node* head = head_.load(std::memory_order_relaxed);
    Node* next = head->next.load(std::memory_order_acquire);    // ①

    if (next == nullptr) return std::nullopt;                    // ②

    T item = std::move(next->data);
    head_.store(next, std::memory_order_relaxed);                // ③
    delete head;                                                 // ④
    size_.fetch_sub(1, std::memory_order_relaxed);
    return item;
}
```

| Line | What happens |
|------|-------------|
| ① | `acquire` — must see the data the producer wrote into `next->data` before publishing the `next` pointer. |
| ② | If `next` is null, queue is empty (only the dummy node left). |
| ③ | Advance head to the next node. The old head becomes garbage. |
| ④ | Delete the *old* dummy node. The next node is now the new dummy. |

### 3.4 When Would It Break?

If multiple consumers call `pop()`, two threads could read the same `head→next` and both try to dequeue the same item → double-free + data corruption.

**Rule: This MPSC queue requires a single consumer.** That consumer is the Dispatcher's dispatch loop.

---

## 4. Lock-Free Deduplication Map (`LockFreeDeduplicator`)

**Files:** `include/eventstream/core/queues/lock_free_dedup.hpp`, `src/core/queues/lock_free_dedup.cpp`

### 4.1 Design

A **concurrent hash map** with 4096 buckets, each an `atomic<Entry*>` pointing to a linked list:

```
buckets_[0]  → entry(id=100, ts=...) → entry(id=4096, ts=...) → nullptr
buckets_[1]  → nullptr
buckets_[2]  → entry(id=2, ts=...) → nullptr
...
buckets_[4095] → ...
```

Hash function: `event_id % num_buckets` (simple, but good enough for uint32_t IDs).

### 4.2 Lock-Free Read Path

```cpp
bool is_duplicate(uint32_t event_id, uint64_t now_ms) {
    size_t bucket_idx = event_id % buckets_.size();
    Entry* entry = buckets_[bucket_idx].load(std::memory_order_acquire);  // ①
    while (entry) {
        if (entry->id == event_id) return true;
        entry = entry->next;                                               // ②
    }
    return false;
}
```

- ① `acquire` ensures we see the complete Entry struct written by an inserter.
- ② Plain pointer chase — safe because entries are only *appended* (never moved), and cleanup only removes old entries.

### 4.3 CAS Insert

```cpp
bool insert(uint32_t event_id, uint64_t now_ms) {
    size_t bucket_idx = event_id % buckets_.size();
    Entry* new_entry = new Entry(event_id, now_ms);
    int retry_count = 0;

    while (retry_count < 3) {
        Entry* head = buckets_[bucket_idx].load(std::memory_order_acquire);

        // Check for duplicate while we have the head
        Entry* curr = head;
        while (curr) {
            if (curr->id == event_id) { delete new_entry; return false; }
            curr = curr->next;
        }

        new_entry->next = head;  // link new entry to current chain

        if (buckets_[bucket_idx].compare_exchange_strong(
                head, new_entry,
                std::memory_order_acq_rel,       // success: publish
                std::memory_order_acquire)) {     // failure: re-read
            return true;
        }
        retry_count++;  // another thread modified the bucket — retry
    }
    delete new_entry;
    return false;
}
```

**CAS loop explained:**

1. Read current head.
2. Scan chain for duplicates.
3. Set `new_entry->next = head` (prepend).
4. `compare_exchange_strong(&head, new_entry)`:
   - If `buckets_[i]` still equals `head` → swap to `new_entry` ✓
   - If another thread changed it → `head` is updated to the new value, retry.

**Why retry limit = 3?** Under high contention, unlimited retries waste CPU. After 3 failures, we log a warning and give up — the event may get processed again (acceptable for best-effort dedup).

### 4.4 Cleanup (Eviction)

Entries older than `IDEMPOTENT_WINDOW_MS` (1 hour) are removed:

```cpp
void cleanup(uint64_t now_ms) {
    for (size_t i = 0; i < buckets_.size(); ++i) {
        cleanup_bucket_count(i, now_ms);  // CAS-based removal of head entries
                                          // + simple unlink for mid-chain entries
    }
}
```

Head removal uses CAS (another thread could be inserting). Mid-chain removal is safe because only one cleanup thread runs, and readers traverse forward — once `prev->next` is updated, new readers skip the removed node. Old readers that already followed the `next` pointer are safe because the Entry isn't freed until after a `seq_cst` fence:

```cpp
prev->next = next;
std::atomic_thread_fence(std::memory_order_seq_cst);
delete curr;
```

**⚠️ This is not fully safe under concurrent reads** — a reader could be dereferencing `curr` while cleanup deletes it. In production, you'd use **epoch-based reclamation (EBR)** or **hazard pointers**. This is a known trade-off documented for interview discussion.

---

## 5. Memory Ordering Cheat Sheet

| Order | Guarantee | Use Case in Project |
|-------|-----------|---------------------|
| `relaxed` | No ordering, just atomicity | Counters (`total_events_processed`), size estimates |
| `acquire` | Reads after this see writes released by another thread | Consumer reading `head_` in SPSC, reading bucket head in dedup |
| `release` | Writes before this are visible to acquiring threads | Producer publishing `head_` in SPSC, inserting dedup entry |
| `acq_rel` | Both acquire and release | `exchange()` in MPSC push, CAS in dedup insert |
| `seq_cst` | Total global order among all `seq_cst` operations | Fence before `delete` in cleanup (defensive) |

### x86 Mapping

On x86-64 (most servers), `acquire` and `release` are free (x86 has a strong memory model — only store→load reordering). Only `seq_cst` adds an `mfence` instruction. On ARM/RISC-V, `acquire`/`release` emit actual barrier instructions, making the distinction performance-critical.

---

## 6. Atomic Operations Used

| Atomic Op | C++ API | CPU Instruction (x86) | Where Used |
|-----------|---------|----------------------|------------|
| Load | `.load(order)` | plain `mov` (acquire = no fence on x86) | SPSC head/tail check |
| Store | `.store(val, order)` | plain `mov` (release = no fence on x86) | SPSC head/tail advance |
| Fetch-add | `.fetch_add(1, relaxed)` | `lock xadd` | Metric counters |
| Exchange | `.exchange(val, acq_rel)` | `lock xchg` | MPSC tail swap |
| CAS | `.compare_exchange_strong(...)` | `lock cmpxchg` | Dedup insert |

---

## 7. Deep Dive: CPU Cache Coherence Protocol (MESI)

Understanding *why* lock-free code works requires understanding the hardware.

### 7.1 MESI States

Every cache line (64 bytes on x86-64) exists in one of four states **per core**:

| State | Meaning | Can Read? | Can Write? |
|-------|---------|-----------|------------|
| **M** (Modified) | Only this core has it; dirty | Yes | Yes |
| **E** (Exclusive) | Only this core has it; clean | Yes | Yes (transitions to M) |
| **S** (Shared) | Multiple cores have it; clean | Yes | No (must invalidate others first) |
| **I** (Invalid) | Not in this core's cache | No | No |

### 7.2 What Happens During SPSC Push/Pop

```
Core 0 (Producer)              Core 1 (Consumer)
─────────────────              ─────────────────
1. Write buffer_[idx]          
   → cache line for buffer_[idx]: M on Core0

2. Store head_ (release)       
   → head_ cache line: M on Core0
   → Core1's head_ copy: I (invalidated)

                               3. Load head_ (acquire)
                               → Bus request: "give me head_"
                               → Core0 flushes M→S, Core1 gets S
                               → Core1 now sees updated head_

                               4. Read buffer_[idx]
                               → Bus request for buffer_ cache line
                               → Core0: M→S, Core1: I→S
                               → Core1 reads the data the producer wrote
```

**The acquire/release pair creates a "happens-before" chain.** Core 1's `acquire` load of `head_` guarantees it sees everything Core 0 wrote *before* the `release` store — including the buffer write at step 1.

### 7.3 Cost Model

| Operation | Cycles (approx, x86-64) | Notes |
|-----------|------------------------|-------|
| L1 cache hit | 4 cycles | Same core, no contention |
| L2 cache hit | 12 cycles | Same core |
| L3 cache hit (same socket) | 40 cycles | Shared across cores |
| Cache-to-cache transfer (same socket) | 40–80 cycles | MESI S→I transition |
| Remote NUMA access | 100–300 cycles | Cross-socket interconnect |
| `lock cmpxchg` (uncontended) | 20 cycles | CAS with no contention |
| `lock cmpxchg` (contended, 8 cores) | 200+ cycles | Cache-line bouncing |
| `lock xchg` (uncontended) | 20 cycles | MPSC exchange |
| `mfence` | 33 cycles | Full memory barrier |

**Key takeaway:** The SPSC buffer costs ~80 cycles per push+pop (two cache-line transfers: head and buffer). A mutex costs 100+ cycles *uncontended* (futex syscall) and 1000+ cycles contended.

---

## 8. Formal Memory Ordering: Happens-Before Proofs

### 8.1 SPSC Correctness Proof

We must prove: *If the consumer reads an item from `buffer_[i]`, it sees the value the producer wrote.*

**Proof by happens-before chain:**

1. Producer: `buffer_[head] = item` (③) **sequenced-before** `head_.store(next, release)` (④).  
   → By C++ sequencing rules, ③ happens-before ④.

2. `head_.store(next, release)` (④) **synchronizes-with** `head_.load(acquire)` (⑥).  
   → By the acquire/release rule: if an `acquire` load reads a value written by a `release` store, all writes before the release are visible after the acquire.

3. Consumer: `head_.load(acquire)` (⑥) **sequenced-before** `buffer_[tail]` read (⑦).  
   → By C++ sequencing rules, ⑥ happens-before ⑦.

4. **Combining:** ③ → ④ → ⑥ → ⑦. By transitivity of happens-before, ③ happens-before ⑦. ∎

### 8.2 MPSC Correctness Proof (Abbreviated)

1. Producer: `Node(item)` constructor **sequenced-before** `tail_.exchange(node, acq_rel)` (②).
2. `tail_.exchange` has `acq_rel` — it synchronizes with the *next* producer's exchange (serialization point).
3. Producer: `prev->next.store(node, release)` (③) publishes the node.
4. Consumer: `head->next.load(acquire)` (pop ①) synchronizes with ③.
5. Consumer: `std::move(next->data)` is sequenced after the acquire load.

**Chain:** constructor → exchange → next.store(release) ↔ next.load(acquire) → move(data). ∎

### 8.3 Litmus Test: What If We Used `relaxed` Everywhere?

Consider this scenario on a **weakly-ordered CPU** (ARM, RISC-V):

```
Thread 0 (Producer):        Thread 1 (Consumer):
  buffer_[0] = 42;           if (head_.load(relaxed) == 1) {
  head_.store(1, relaxed);     r = buffer_[0];  // could read 0!
                              }
```

On ARM, the CPU can reorder the `buffer_` write after the `head_` store. Thread 1 sees `head_ == 1` but `buffer_[0]` hasn't been written yet. **This is a real bug** — not hypothetical. ARM's weak memory model allows store-store reordering.

On x86, this particular reorder can't happen (TSO guarantees store-store order), but the C++ standard doesn't guarantee x86. Always program to the standard, not the hardware.

---

## 9. Compiler Barriers vs Hardware Barriers

### 9.1 The Compiler Can Reorder Too

Even if the CPU executes instructions in order, the **compiler** may reorder code for optimization:

```cpp
// Source code:
buffer_[idx] = item;
head_.store(next, std::memory_order_release);

// Compiler COULD generate (without barriers):
head_.store(next);      // moved up!
buffer_[idx] = item;    // too late - consumer already saw head
```

`std::memory_order_release` prevents this: it tells the compiler "do not move any writes past this store."

### 9.2 Barrier Instruction Mapping (x86-64)

| C++ Memory Order | Compiler Barrier? | CPU Instruction |
|-------------------|-------------------|----------------|
| `relaxed` | No | plain `mov` |
| `acquire` (load) | Yes (compiler fence) | plain `mov` (x86 TSO gives acquire for free) |
| `release` (store) | Yes (compiler fence) | plain `mov` (x86 TSO gives release for free) |
| `acq_rel` (RMW) | Yes | `lock` prefix (e.g., `lock xchg`) |
| `seq_cst` (load) | Yes | `mov` (same as acquire on x86 for loads) |
| `seq_cst` (store) | Yes | `mov` + `mfence` (or `lock xchg rsp` trick) |

**On x86-64, acquire/release are "free" in hardware.** The compiler barrier prevents reordering at compile time; the CPU naturally provides the ordering at runtime. This is why the SPSC buffer has essentially zero overhead on x86.

### 9.3 ARM64 Barrier Mapping (for comparison)

| C++ Memory Order | ARM64 Instruction |
|-------------------|-------------------|
| `relaxed` | plain `ldr` / `str` |
| `acquire` (load) | `ldar` (load-acquire) |
| `release` (store) | `stlr` (store-release) |
| `acq_rel` (RMW) | `ldaxr` + `stlxr` (LL/SC pair) |
| `seq_cst` | `ldar`/`stlr` + `dmb ish` |

On ARM, every acquire/release has **real cost** (~15 cycles for `ldar/stlr`). This makes the choice of memory ordering performance-critical on ARM, whereas on x86 it's mostly about correctness.

---

## 10. Lock-Free Progress Guarantees

| Guarantee | Definition | Our Structures |
|-----------|-----------|----------------|
| **Wait-free** | Every thread completes in bounded steps | MPSC push (`exchange` is one instruction) |
| **Lock-free** | At least one thread makes progress | SPSC push/pop, Dedup `is_duplicate` |
| **Obstruction-free** | A thread makes progress if no others interfere | Dedup `insert` (CAS retry) |
| **Blocking** | No guarantee; a stalled thread can block all | `std::mutex` in EventBus deques |

**SPSC** is lock-free (not wait-free) because `push` can return `false` (full) and `pop` can return `nullopt` (empty) — the *caller* retries, but the operation itself always completes.

**MPSC push** is **wait-free**: `exchange` always succeeds in exactly one atomic instruction. No retry loop. Even under maximum contention (100 threads pushing simultaneously), every thread completes in O(1).

**Dedup insert** is **obstruction-free**: the CAS can fail if another thread modifies the bucket concurrently. With the 3-retry limit, it can give up. True lock-freedom would require unbounded retries.

---

## 11. Advanced: The Vyukov MPSC Inconsistency Window

There's a subtle issue in the MPSC queue that interviewers love to ask about:

```cpp
// Producer:
Node* prev = tail_.exchange(node, acq_rel);  // Step A: atomic
prev->next.store(node, release);              // Step B: NOT atomic with A
```

Between Step A and Step B, there's a **window** where:
- `tail_` points to the new node.
- But `prev->next` is still `nullptr`.

If the consumer tries to pop during this window:

```cpp
// Consumer:
Node* next = head->next.load(acquire);  // Returns nullptr!
```

The consumer sees an empty queue even though an item was just pushed. This is **not a bug** — it's a deliberate design choice:

1. The consumer returns `nullopt` and tries again on the next iteration.
2. By that time, Step B has completed and `next` is visible.
3. The "lost" item is recovered within one retry cycle (microseconds).

**Interview answer:** "The Vyukov MPSC queue has a brief inconsistency window between the exchange and the next-pointer link. The consumer handles this by returning empty and retrying. This is acceptable because the dispatcher loop already polls in a tight loop with a 100μs sleep. The alternative — making push a single atomic operation — would require a more complex (and slower) algorithm like a Michael-Scott queue."

---

## 12. Advanced: Dedup Map Safe Memory Reclamation Problem

The current dedup cleanup has a known theoretical issue:

```cpp
// Cleanup thread:
prev->next = entry->next;     // Unlink entry
delete entry;                  // Free memory

// Concurrent reader (in is_duplicate):
entry = entry->next;           // Might dereference freed memory!
```

**The read could crash** if the cleanup deletes an entry while a reader is traversing it. This is the classic **safe memory reclamation** problem in lock-free programming.

### Solutions (in order of complexity):

| Technique | How It Works | Overhead |
|-----------|-------------|---------|
| **RCU (Read-Copy-Update)** | Readers enter "read sections"; deletions deferred until all readers exit | Near-zero for reads; Linux kernel uses this |
| **Hazard Pointers** | Each reader publishes pointers it's using; cleanup skips those | One atomic write per read |
| **Epoch-Based Reclamation** | Global epoch counter; free memory 2 epochs later | One atomic read per operation |
| **Quiescent-State-Based** | Wait until all threads pass a quiescent point | Zero overhead for reads |

**Why the project doesn't use these:** The cleanup runs once per hour. The probability of a reader being in the exact nanosecond window where an entry is deleted is astronomically low (~1 in 10^9 per cleanup cycle). For a non-safety-critical system, this pragmatic trade-off is acceptable. In an interview, **acknowledge the issue and explain the mitigation.**

---

## 13. Performance Analysis: Benchmark Data Interpretation

The project includes benchmarks in `benchmark/benchmark_spsc.cpp` and `benchmark/benchmark_mpsc.cpp`. Here's how to interpret results:

### SPSC Throughput Model

```
Throughput = Capacity / (push_time + pop_time + cache_transfer_time)
```

On a modern Xeon:
- Push: ~5ns (write buffer + release store)
- Pop: ~5ns (acquire load + read buffer)
- Cache transfer: ~40ns (head/tail cache lines)
- **Effective per-item cost: ~10-15ns → ~70-100M events/sec**

### MPSC Throughput Under Contention

```
Throughput_per_producer = 1 / (exchange_time × contention_factor)
```

- 1 producer: ~20ns/op → 50M ops/sec
- 2 producers: ~40ns/op → 25M ops/sec each (50M total)
- 8 producers: ~160ns/op → 6M ops/sec each (48M total)

Contention scales linearly because `exchange` serializes on the same cache line. Total throughput plateaus around 50M ops/sec regardless of producer count.

### Dedup Under Contention

- Sequential: ~30ns per insert (CAS + hash + chain traversal)
- 8-thread contention on same bucket: ~500ns (CAS retries dominate)
- 8-thread, random buckets (4096): ~50ns (minimal contention)

**Key insight for interview:** "Lock-free doesn't mean contention-free. When multiple threads compete for the same cache line, even lock-free algorithms slow down due to cache-line bouncing. The performance advantage is that no thread is *blocked* — they all make progress, just more slowly."

---

## 14. Common Interview Questions (Extended)

**Q1: Explain the difference between `compare_exchange_weak` and `_strong`.**  
A: `weak` can spuriously fail on LL/SC architectures (ARM, RISC-V) because the store-conditional can fail if the cache line was evicted for any reason (not just another write). Use `weak` inside a loop (it's faster on ARM because it doesn't need to suppress spurious failures). Use `strong` when a single failure means something (e.g., bounded retry).  
→ Project uses `_strong` with retry limit of 3 in dedup insert.

**Q2: What is the ABA problem? Does this project suffer from it?**  
A: ABA: Thread reads A, another thread changes A→B→A, original thread's CAS succeeds incorrectly.  
→ SPSC: No ABA (monotonically increasing indices — never reuses a value).  
→ MPSC: No ABA (`exchange` replaces unconditionally; no CAS).  
→ Dedup: Potential ABA if an entry is deleted and a new entry with the same ID is allocated at the same address. Mitigated by (1) 1-hour window making deletion rare, (2) IDs are uint32_t while addresses are 64-bit (extremely unlikely same address reuse).

**Q3: Why `alignas(64)` for head and tail?**  
A: Prevents false sharing. Without it, `head_` and `tail_` could be on the same 64-byte cache line. When the producer writes `head_`, the consumer's core invalidates its copy of `tail_` even though `tail_` didn't change. This turns a lock-free algorithm into a cache-line ping-pong. Measured impact: **2-3× throughput reduction** without alignment.

**Q4: Can you replace the SPSC ring buffer with a `std::deque`?**  
A: Functionally yes, but `std::deque` requires a mutex, performs heap allocations on insertion (deque uses fixed-size blocks with pointer indirection), and has non-sequential memory access (cache-unfriendly). The SPSC ring buffer: **zero allocations**, **zero locks**, **zero system calls**, and **sequential memory access** (prefetcher-friendly).

**Q5: Why is the MPSC queue using `new` for every push?**  
A: Each node must be independently addressable and linked via `next` pointer. Unlike SPSC (fixed ring with pre-allocated slots), MPSC can't predict which slot to use because multiple producers race. Solutions: (1) per-thread node pool (eliminates `new` on hot path), (2) intrusive approach (embed `Node` in the Event struct — zero allocations, but couples data structure to event layout).

**Q6: What is a "memory fence" at the hardware level?**  
A: On x86, `mfence` drains the store buffer — all pending stores become visible to all cores before any subsequent load executes. Cost: ~33 cycles. On ARM, `dmb ish` (Data Memory Barrier, Inner Shareable) ensures ordering across all cores in the same shareability domain. Fences are the most expensive synchronization primitive; acquire/release avoids them on x86.

**Q7: Why does the SPSC use `size_t` for head/tail instead of wrapping around?**  
A: Head and tail are monotonically increasing. Wrapping is done at access time: `buffer_[head & (Capacity-1)]`. This avoids the ABA problem entirely — head value 16384 and head value 0 are distinguishable even though they map to the same slot. With `uint64_t`, wrap-around occurs at 2^64 events (~584 years at 1 billion events/sec). Effectively infinite.

**Q8: What is "sequential consistency" and why do we avoid it?**  
A: `seq_cst` guarantees a single total order of all `seq_cst` operations across all threads. This requires an `mfence` on every store (x86) or `dmb` on every load (ARM). For producer-consumer patterns, `acquire/release` is sufficient because there are only two threads and one synchronization variable. `seq_cst` is needed when three or more threads must agree on the order of two or more independent atomic variables (Dekker's algorithm, etc.).

**Q9: Draw the happens-before graph for a single event flowing through the MPSC queue.**  

```
Producer Thread:                   Consumer Thread:
                                   
  new Node(item)                   
       │ sequenced-before          
       ▼                           
  tail_.exchange(node, acq_rel) ─── synchronizes-with ──┐
       │ sequenced-before                                │
       ▼                                                 │
  prev->next.store(node, release) ─ synchronizes-with ─┐│
                                                        ││
                                   head->next.load(acquire)
                                         │ sequenced-before
                                         ▼
                                   std::move(next->data)
```

**Q10: What would happen if you added a second consumer to the MPSC queue?**  
A: Two consumers could both read `head->next`, both get the same node, both move the data (double-move → UB), and both delete the old dummy (double-free → crash). Fix: protect `pop()` with a mutex (making it MPMC, which defeats the purpose) or use a proper MPMC queue (e.g., bounded array-based with `fetch_add` for head and tail).
