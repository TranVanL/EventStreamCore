# 🔗 Lock-Free Queues

> Deep dive into SPSC, MPSC và Lock-Free Dedup implementations.

---

## 🎯 Overview

| Component | File | Type | Capacity | Latency | Use Case |
|-----------|------|------|----------|---------|----------|
| SpscRingBuffer | spsc_ring_buffer.hpp | Ring array | 16384 | ~8ns | REALTIME queue |
| MpscQueue | mpsc_queue.hpp | Linked nodes | 65536 | ~20ns | TRANS/BATCH queues |
| LockFreeDeduplicator | lock_free_dedup.hpp | Hash buckets | 4096 | ~14ns | Idempotency check |

---

## 📦 SpscRingBuffer

### Design

```
┌─────────────────────────────────────────────────────────────────────┐
│                        SPSC RING BUFFER                              │
│                                                                      │
│     head_ (producer)                    tail_ (consumer)            │
│         │                                    │                       │
│         ▼                                    ▼                       │
│     ┌───────────────────────────────────────────────────────────┐   │
│     │ 0 │ 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │ 8 │ 9 │...│ N │         │   │
│     └───────────────────────────────────────────────────────────┘   │
│         ▲                   ▲               ▲                       │
│         │                   │               │                       │
│      Empty slots         Data slots      Wrap around               │
│                                                                      │
│  Capacity: 16384 (power of 2 for fast modulo via bitmask)          │
│  Index calculation: index & (Capacity - 1) instead of %            │
└─────────────────────────────────────────────────────────────────────┘
```

### Implementation (from code)

```cpp
// File: include/eventstream/core/queues/spsc_ring_buffer.hpp
template<typename T, size_t Capacity>
class SpscRingBuffer {
public:
    bool push(const T& item);
    std::optional<T> pop();
    size_t SizeUsed() const;

private:
    // Cache-line aligned to prevent false sharing
    alignas(64) T buffer_[Capacity];          // Data storage
    alignas(64) std::atomic<size_t> head_{0}; // Producer writes
    alignas(64) std::atomic<size_t> tail_{0}; // Consumer writes
};
```

### Memory Ordering

| Operation | Memory Order | Reason |
|-----------|--------------|--------|
| head_.load() (producer) | relaxed | Only producer writes head |
| tail_.load() (producer) | acquire | Sync with consumer's release |
| head_.store() | release | Publish data before head update |
| tail_.load() (consumer) | relaxed | Only consumer writes tail |
| head_.load() (consumer) | acquire | Sync with producer's release |
| tail_.store() | release | Free slot visible to producer |

### Push Sequence

```
Producer Thread:
┌─────────────────────────────────────────────────────────────┐
│ 1. head = head_.load(relaxed)     // Fast read              │
│ 2. next = (head + 1) & mask       // Wrap with bitmask      │
│ 3. if (next == tail_.load(acquire)) return false  // Full!  │
│ 4. buffer_[head] = std::move(item)  // Write data           │
│ 5. head_.store(next, release)     // Publish                │
└─────────────────────────────────────────────────────────────┘

Visual:
Before:  tail=2           head=5
         │                 │
         ▼                 ▼
     ┌───┬───┬───┬───┬───┬───┬───┐
     │ - │ - │ A │ B │ C │ - │ - │
     └───┴───┴───┴───┴───┴───┴───┘
              ═══════════
                 Data

After push(D):
         tail=2               head=6
         │                     │
         ▼                     ▼
     ┌───┬───┬───┬───┬───┬───┬───┐
     │ - │ - │ A │ B │ C │ D │ - │
     └───┴───┴───┴───┴───┴───┴───┘
              ═══════════════
                   Data
```

### Pop Sequence

```
Consumer Thread:
┌─────────────────────────────────────────────────────────────┐
│ 1. tail = tail_.load(relaxed)     // Fast read              │
│ 2. if (tail == head_.load(acquire)) return nullopt // Empty │
│ 3. item = std::move(buffer_[tail])  // Read data            │
│ 4. tail_.store((tail + 1) & mask, release)  // Advance      │
└─────────────────────────────────────────────────────────────┘
```

---

## 📦 MpscQueue (Vyukov Algorithm)

### Design

```
┌─────────────────────────────────────────────────────────────────────┐
│                      MPSC VYUKOV QUEUE                               │
│                                                                      │
│  Multiple producers can push concurrently (lock-free via exchange)  │
│  Single consumer pops (no contention)                               │
│                                                                      │
│  Key insight: Use atomic exchange on tail_ to serialize pushes      │
│  without locks. Each producer atomically claims a position, then    │
│  links the previous node.                                           │
└─────────────────────────────────────────────────────────────────────┘
```

### Implementation (from code)

```cpp
// File: include/eventstream/core/queues/mpsc_queue.hpp
template<typename T, size_t Capacity = 65536>
class MpscQueue {
public:
    MpscQueue() : size_(0) {
        // Initialize with dummy node
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }
    
    bool push(const T& item);      // Thread-safe for multiple producers
    std::optional<T> pop();        // Single consumer only
    size_t size() const;
    bool empty() const;

private:
    struct Node {
        T data;
        std::atomic<Node*> next{nullptr};
        Node() = default;
        explicit Node(const T& d) : data(d) {}
    };
    
    std::atomic<Node*> head_;      // Consumer reads from here
    std::atomic<Node*> tail_;      // Producers push here
    std::atomic<size_t> size_;     // Approximate count
};
```

### Push Algorithm

```
Push Operation (Multiple Producers):
┌─────────────────────────────────────────────────────────────┐
│ 1. Check capacity: if (size_ >= Capacity) return false      │
│ 2. Node* node = new Node(item)                              │
│ 3. prev = tail_.exchange(node, acq_rel)  // Atomic claim!   │
│ 4. prev->next.store(node, release)       // Link previous   │
│ 5. size_.fetch_add(1, relaxed)                              │
└─────────────────────────────────────────────────────────────┘

Visualization:

Before (2 producers racing):
    head_────►┌─────────┐◄────tail_
              │  DUMMY  │
              │next:null│
              └─────────┘

After Producer 1 exchange (claims tail):
    head_────►┌─────────┐      ┌─────────┐◄────tail_
              │  DUMMY  │      │ NODE A  │
              │next: ?  │      │next:null│
              └─────────┘      └─────────┘

After Producer 1 links:
    head_────►┌─────────┐────►┌─────────┐◄────tail_
              │  DUMMY  │      │ NODE A  │
              │next: A  │      │next:null│
              └─────────┘      └─────────┘
```

### Pop Algorithm

```
Pop Operation (Single Consumer):
┌─────────────────────────────────────────────────────────────┐
│ 1. head = head_.load(relaxed)                               │
│ 2. next = head->next.load(acquire)                          │
│ 3. if (next == nullptr) return nullopt  // Empty!           │
│ 4. item = std::move(next->data)         // Get data         │
│ 5. head_.store(next, release)           // Advance head     │
│ 6. delete head                          // Free old dummy   │
│ 7. size_.fetch_sub(1, relaxed)                              │
└─────────────────────────────────────────────────────────────┘

Key: The consumed node becomes the new dummy node!
```

---

## 📦 LockFreeDeduplicator

### Design

```
┌─────────────────────────────────────────────────────────────────────┐
│                    LOCK-FREE DEDUPLICATOR                            │
│                                                                      │
│  Hash-based deduplication for idempotent event processing           │
│                                                                      │
│  Features:                                                          │
│  • 4096 buckets (configurable)                                      │
│  • CAS-based insertion (no locks in hot path)                       │
│  • 1-hour idempotency window (IDEMPOTENT_WINDOW_MS = 3600000)       │
│  • Separate cleanup thread for expired entries                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Implementation (from code)

```cpp
// File: include/eventstream/core/queues/lock_free_dedup.hpp
namespace EventStream {

class LockFreeDeduplicator {
public:
    struct Entry {
        uint32_t id;
        uint64_t timestamp_ms;
        Entry* next;  // Chain for collisions
    };
    
    static constexpr size_t DEFAULT_BUCKETS = 4096;
    static constexpr uint64_t IDEMPOTENT_WINDOW_MS = 3600000;  // 1 hour
    
    // Lock-free read: O(chain length), typically O(1)
    bool is_duplicate(uint32_t event_id, uint64_t now_ms);
    
    // CAS-based insert: Returns true if new, false if duplicate
    bool insert(uint32_t event_id, uint64_t now_ms);
    
    // Background cleanup: Remove entries older than window
    void cleanup(uint64_t now_ms);
    
private:
    std::vector<std::atomic<Entry*>> buckets_;
};

}  // namespace EventStream
```

### Hash & Lookup

```
is_duplicate(event_id, now_ms):
┌─────────────────────────────────────────────────────────────┐
│ 1. bucket_idx = event_id % buckets_.size()                  │
│ 2. entry = buckets_[bucket_idx].load(acquire)               │
│ 3. while (entry != nullptr):                                │
│    • if (entry->id == event_id) return true  // DUPLICATE!  │
│    • entry = entry->next                                    │
│ 4. return false  // NEW EVENT                               │
└─────────────────────────────────────────────────────────────┘

Bucket Layout:
                    event_id % 4096 = bucket index
                              │
                              ▼
Buckets:  [0] ──► Entry(id=4096) ──► Entry(id=8192) ──► null
          [1] ──► Entry(id=1) ──► null
          [2] ──► null
          [3] ──► Entry(id=3) ──► Entry(id=4099) ──► null
          ...
```

### CAS-based Insert

```
insert(event_id, now_ms):
┌─────────────────────────────────────────────────────────────┐
│ 1. bucket_idx = event_id % buckets_.size()                  │
│ 2. Entry* new_entry = new Entry(event_id, now_ms)           │
│ 3. do {                                                     │
│       expected = buckets_[bucket_idx].load(acquire)         │
│       // Check if already exists in chain                   │
│       if (find_in_chain(expected, event_id))                │
│           delete new_entry; return false // DUPLICATE       │
│       new_entry->next = expected                            │
│    } while (!buckets_[bucket_idx].compare_exchange_weak(    │
│        expected, new_entry, release, relaxed))              │
│ 4. return true  // INSERTED                                 │
└─────────────────────────────────────────────────────────────┘
```

### Cleanup (Background Thread)

```
cleanup(now_ms):  // Called periodically, NOT in hot path
┌─────────────────────────────────────────────────────────────┐
│ for each bucket:                                            │
│   // Use CAS to safely update head when removing expired    │
│   // Traverse chain, remove entries where:                  │
│   //   now_ms - entry->timestamp_ms > IDEMPOTENT_WINDOW_MS  │
│                                                             │
│ BUG FIXED: cleanup_bucket_count() now uses CAS for head     │
│            update to prevent race condition                 │
└─────────────────────────────────────────────────────────────┘
```

---

## 🔄 Usage in EventBusMulti

```cpp
// EventBusMulti queue configuration
class EventBusMulti {
private:
    // REALTIME: Lock-free, wait-free, ~8ns
    // Uses SPSC because single ingest thread → single processor thread
    struct RealtimeQueue {
        SpscRingBuffer<EventPtr, 16384> ringBuffer;
        OverflowPolicy policy = OverflowPolicy::DROP_OLD;
        std::atomic<PressureLevel> pressure{PressureLevel::NORMAL};
    };

    // TRANSACTIONAL/BATCH: Lock-free multi-producer
    // Multiple ingest threads can push concurrently
    struct Q {
        mutable std::mutex m;           // For condition_variable
        std::condition_variable cv;      // For blocking pop
        std::deque<EventPtr> dq;         // Fallback (could use MpscQueue)
        size_t capacity = 0;
        OverflowPolicy policy;
    };
    
    RealtimeQueue RealtimeBus_;   // CRITICAL/HIGH priority
    Q TransactionalBus_;          // MEDIUM priority  
    Q BatchBus_;                  // LOW/BATCH priority
};
```

---

## ⚠️ Known Limitations

| Component | Limitation | Mitigation |
|-----------|------------|------------|
| SpscRingBuffer | Fixed capacity (16384) | Power of 2 allows bitmask modulo |
| MpscQueue | Node allocation per push | Could add node pool for hot path |
| LockFreeDedup | Chain length under collision | 4096 buckets keeps chains short |
| LockFreeDedup | Cleanup not lock-free | Runs in background thread, not hot path |

---

## ➡️ Next

- [Memory Pools & NUMA →](memory.md)
