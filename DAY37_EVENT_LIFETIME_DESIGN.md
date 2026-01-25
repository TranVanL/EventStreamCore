# Day 37: Event Lifetime & Ownership Model Design

## Overview

This document describes the ownership model, memory safety, and concurrency guarantees for EventStreamCore. It addresses:
- Who owns Event objects?
- When are Events recycled/freed?
- How do we avoid ABA problems in lock-free structures?
- How do we prevent use-after-free (UAF) without shared ownership complexity?

---

## 1. Ownership Model: Single Consumer Per Queue

### Principle
**"We avoid shared ownership instead of solving it."** (Senior-level insight)

Each event queue has a **single consumer thread**:

```
Producer threads (many)  ---push---->  [Queue]  ---pop---->  Single Consumer Thread
                                                               (only one reader)
```

### Benefit
- No concurrent readers → No reader synchronization needed
- No shared ownership required
- Lock-free pop() is trivial (only tail pointer matters)
- Significant latency reduction vs. lock-based queues

### Implementation
- **REALTIME queue**: SpscRingBuffer (Single-Producer, Single-Consumer)
  - Multiple producers can push (atomic head increment)
  - Only one thread can pop (tail is owned by consumer)

- **TRANSACTIONAL/BATCH queues**: Mutex-protected deque
  - Multiple producers with lock
  - Single consumer thread per processor (lock only for batch operations)

---

## 2. Event Lifetime & Recycling

### Lifecycle Phases

```
┌────────────────────────────────────────────────────────────────┐
│  Phase 1: Creation (TCP Parser)                                │
│  - Allocate Event on heap via EventFactory                     │
│  - Wrap in shared_ptr<Event> (reference count = 1)             │
└────────────────────────────────────────────────────────────────┘
                             ↓
┌────────────────────────────────────────────────────────────────┐
│  Phase 2: In Queue (EventBusMulti)                             │
│  - Event held in queue container (ref count ≥ 1)              │
│  - On pop(): dequeue_time_ns is set, passed to consumer      │
│  - Ref count still ≥ 1 (queue + consumer both hold)           │
└────────────────────────────────────────────────────────────────┘
                             ↓
┌────────────────────────────────────────────────────────────────┐
│  Phase 3: Processing (EventProcessor)                          │
│  - Consumer holds const EventPtr& (no new reference)           │
│  - Process event (record latency, check duplicates, etc.)     │
│  - Once process() returns, consumer releases reference         │
│  - Ref count drops (queue already released it via pop())      │
└────────────────────────────────────────────────────────────────┘
                             ↓
┌────────────────────────────────────────────────────────────────┐
│  Phase 4: Destruction                                          │
│  - When ref count reaches 0, destructor called automatically   │
│  - Memory reclaimed by allocator                               │
│  - No manual recycling needed (RAII)                          │
└────────────────────────────────────────────────────────────────┘
```

### Key Points
1. **No Manual Recycling**: We use `std::shared_ptr` for automatic lifetime management
2. **Bounded Lifetime**: Event lives only while in queue OR being processed
3. **No Dangling Pointers**: Ref counting prevents all UAF cases
4. **Single Consumer Scope**: Event lifetime is naturally bounded by consumer's processing function

---

## 3. Lock-Free Queue: How It Works

### SPSC Ring Buffer Layout
```
Consumer (tail):  Reads from tail_, updates tail_ after reading
Producer (head):  Writes to head_, updates head_ after writing

Buffer: [0] [1] [2] ... [N-1] [0] [1] ...  (circular)
         ^tail (only consumer moves this)
             ^head (only main producer moves this)
```

### Critical Invariants
1. **No Concurrent Reads**: Only consumer moves `tail_`
   - Reader doesn't need atomics (single observer)
   - Only synchronization: `acquire` when reading `head` (to ensure producer's write visibility)

2. **No Concurrent Writes**: Only producer moves `head_`
   - Writer doesn't need atomics (single observer)
   - Only synchronization: `acquire` when reading `tail` (to ensure consumer's release visibility)

3. **Buffer Size = Power of 2**:
   - Wrap-around: `(index + 1) & (Capacity - 1)` instead of modulo
   - Avoids division, branch-free

### Memory Ordering
```cpp
// Producer (push)
head = head_.load(relaxed);  // No sync needed, only producer reads this
next = (head + 1) & MASK;
if (next == tail_.load(acquire)) {  // Acquire: see consumer's release
    return false;  // Queue full
}
buffer[head] = item;
head_.store(next, release);  // Release: make write visible to consumer

// Consumer (pop)
tail = tail_.load(relaxed);  // No sync needed, only consumer reads this
if (tail == head_.load(acquire)) {  // Acquire: see producer's release
    return nullopt;  // Queue empty
}
item = buffer[tail];
tail_.store((tail + 1) & MASK, release);  // Release: make read visible to producer
```

**Correctness**: Because of single producer/consumer design, we don't need sequential consistency—just acquire/release barriers to ensure visibility across threads.

---

## 4. ABA Problem: Why It's Absent

### What is ABA?
Classic lock-free problem: Compare-and-swap (CAS) reads value A, another thread changes A→B→A, CAS succeeds but state is inconsistent.

### Why SPSC Ring Buffer Avoids It
1. **Single Consumer**: Only consumer advances tail
   - No competing CAS on `tail_`
   - No ABA possible on tail

2. **Head Capacity Check**: `next == tail` check always valid
   - Even if head wraps, tail position is monotonic per consumer
   - Race condition: head and tail both advance, but queue state remains consistent
   - Worst case: brief false "full" detection → no correctness issue

3. **Buffer Index Never Reused While Occupied**:
   ```
   Capacity = 16384
   Head and tail never simultaneously point to same index 
   unless queue is empty → index is never reused while occupied
   ```

### Proof
- Producer advances head_ sequentially (no CAS)
- Consumer advances tail_ sequentially (no CAS)
- Both atomically increment → indices are globally consistent
- ABA would require CAS failure + recovery, which doesn't happen in our design

---

## 5. Memory Ordering & Synchronization

### Store-Release / Load-Acquire Pattern

We use this pattern extensively:

```cpp
// Synchronization point 1: Producer writes
buffer[head] = item;
head_.store(next, std::memory_order_release);
           ↓
           Ensures: buffer write → head update visibility

// Synchronization point 2: Consumer reads
next_head = head_.load(std::memory_order_acquire);
                                        ↓
           Ensures: head read → subsequent buffer read visibility
```

This is **weaker than full sequential consistency** but sufficient because:
- No concurrent readers of same data
- Only need producer → consumer visibility for handoff point

---

## 6. Boundary Conditions & Edge Cases

### Queue Overflow (Backpressure)
- Checked via: `next_head == tail` comparison
- If full:
  - **CRITICAL priority**: Force-push (drop old if needed)
  - **HIGH priority**: Attempt downgrade to TRANSACTIONAL
  - **BATCH priority**: Drop new event
  - See `backpressure_strategy.hpp` for details

### Event Recycling After Overflow
- If event dropped from realtime queue:
  - moved to Dead Letter Queue (DLQ)
  - shared_ptr still holds reference
  - ref count eventually hits 0
  - Destructor runs, memory freed

### Consumer Blocking
- TRANSACTIONAL queue uses `condition_variable` for blocking producers
- Consumer pop() blocks with timeout
- No deadlock possible (single producer per queue scenario)

---

## 7. Race Conditions: Analysis

### Producer vs Producer
❌ **Impossible**: Only main producer thread advances head
- Other producers go through EventBusMulti API
- EventBusMulti ensures single-threaded access to push logic

### Consumer vs Consumer  
❌ **Impossible**: Only consumer thread advances tail
- Contract enforced at API level
- If violated, that's user's bug (not our design's problem)

### Producer vs Consumer
✅ **Handled**: Memory barriers ensure visibility
- acquire/release ordering sufficient
- No CAS-based concurrent updates
- Atomicity of pointers is sufficient (push succeeds or fails atomically)

---

## 8. Summary: Why This Design is Safe

| Guarantee | Mechanism |
|-----------|-----------|
| **No UAF** | `shared_ptr` automatic reclamation + single consumer scope |
| **No data races** | Acquire/release barriers at queue handoff points |
| **No ABA** | No CAS operations; monotonic head/tail indices |
| **No deadlock** | No circular waits; timeout on condition_variable |
| **Lock-free** | No mutex in hot path (SPSC ring buffer); only CV for backpressure |
| **Realtime** | No dynamic allocation in hot path; no GC pauses; bounded latency |

---

## 9. Interview-Ready Explanation

**Q: How do you handle Event ownership without garbage collection?**

A: "We use `shared_ptr` for automatic reference counting. Each Event is owned during its lifetime in the queue and consumer scope. Once the consumer returns from processing, the reference is released and the Event is automatically freed. This is safe because we have a **single consumer per queue**, so we never have multiple threads competing to free the same object."

**Q: What about ABA problems in your lock-free queue?**

A: "We avoid them by design. Our ring buffer is single-producer, single-consumer. The producer only advances the head pointer, and the consumer only advances the tail pointer. Since there's no Compare-and-Swap (CAS) in the critical path—just atomic increments—the ABA problem cannot occur. The worst case is a brief false 'full' detection, but that's handled correctly by the backpressure strategy."

**Q: How do you prevent use-after-free?**

A: "By avoiding shared ownership complexity. Each Event is owned by exactly one entity at a time: either the queue or the consumer. The `shared_ptr` ensures that when the last reference is released, the destructor runs immediately. Since the consumer is single-threaded per queue, we don't have concurrent destruction issues. If a task is dropped due to backpressure, it goes to the Dead Letter Queue and is still tracked—no silent drops."

---

## References

- **Lock-free queue design**: "Correct and Efficient Bounded FIFO Queues" (Mohan et al., 1995)
- **Memory ordering**: C++11 std::memory_order documentation (acquire/release semantics)
- **RAII principle**: "Resource Acquisition Is Initialization" (Stroustrup)
- **EventStreamCore**: Day 34 (lock-free dedup), Day 36 (RAFT cluster)
