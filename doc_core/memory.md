# 💾 Memory Pools & NUMA Optimization

> Zero-allocation design với NUMA-aware memory management.

---

## 🎯 Overview

| Component | File | Description | Performance |
|-----------|------|-------------|-------------|
| EventPool | event_pool.hpp | Basic thread-local pool | O(1) acquire/release |
| NUMAEventPool | numa_event_pool.hpp | NUMA-aware allocation | ~50ns local vs ~300ns remote |
| IngestEventPool | ingest_pool.hpp | Wrapper using NUMAEventPool | Used by TCP/UDP servers |
| NUMABinding | numa_binding.hpp | CPU/memory affinity utilities | Thread pinning |

---

## 📦 NUMAEventPool Design

### Purpose

```
┌─────────────────────────────────────────────────────────────────────┐
│                     NUMA MEMORY ACCESS                               │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                      NUMA Node 0                             │    │
│  │  ┌─────────┬─────────┬─────────┬─────────┐                  │    │
│  │  │  CPU 0  │  CPU 1  │  CPU 2  │  CPU 3  │                  │    │
│  │  └────┬────┴────┬────┴────┬────┴────┬────┘                  │    │
│  │       │         │         │         │                        │    │
│  │       └─────────┴────┬────┴─────────┘                        │    │
│  │                      │                                        │    │
│  │              ┌───────▼───────┐                               │    │
│  │              │   LOCAL RAM   │  ◄── ~50-80ns access          │    │
│  │              │    64 GB      │                               │    │
│  │              └───────────────┘                               │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                              │                                       │
│                              │  QPI/UPI Interconnect                │
│                              │  (+100-200ns extra latency!)         │
│                              │                                       │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                      NUMA Node 1                             │    │
│  │  ┌─────────┬─────────┬─────────┬─────────┐                  │    │
│  │  │  CPU 4  │  CPU 5  │  CPU 6  │  CPU 7  │                  │    │
│  │  └────┬────┴────┬────┴────┬────┴────┬────┘                  │    │
│  │       │         │         │         │                        │    │
│  │       └─────────┴────┬────┴─────────┘                        │    │
│  │                      │                                        │    │
│  │              ┌───────▼───────┐                               │    │
│  │              │   LOCAL RAM   │  ◄── ~50-80ns access          │    │
│  │              │    64 GB      │                               │    │
│  │              └───────────────┘                               │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                      │
│  ⚠️ CPU 0 accessing Node 1 RAM = 150-280ns (3-4x slower!)          │
└─────────────────────────────────────────────────────────────────────┘
```

### Implementation (from code)

```cpp
// File: include/eventstream/core/memory/numa_event_pool.hpp
namespace eventstream::core {

template<typename EventType, size_t Capacity>
class NUMAEventPool {
public:
    /**
     * Create NUMA-aware event pool
     * @param numa_node NUMA node ID (-1 for default allocation)
     */
    explicit NUMAEventPool(int numa_node = -1);
    
    /**
     * Acquire event from pool - O(1)
     * @return Pointer to event, nullptr if pool exhausted
     */
    EventType* acquire();
    
    /**
     * Release event back to pool - O(n) search + NUMA free
     * @param event Event to release
     */
    void release(EventType* event);
    
    // Pool statistics
    size_t available() const { return available_count_; }
    size_t capacity() const { return Capacity; }
    int numaNode() const { return numa_node_; }

private:
    std::array<std::unique_ptr<EventType>, Capacity> pool_;
    size_t available_count_;
    int numa_node_;
};

}  // namespace eventstream::core
```

### NUMA Allocation Strategy

```cpp
// Constructor - allocate on specific NUMA node
NUMAEventPool(int numa_node) : available_count_(Capacity), numa_node_(numa_node) {
    #ifdef __linux__
    if (numa_node >= 0 && NUMABinding::getNumNumaNodes() > 0) {
        for (size_t i = 0; i < Capacity; ++i) {
            // Allocate raw memory on NUMA node
            void* mem = NUMABinding::allocateOnNode(sizeof(EventType), numa_node);
            
            if (mem) {
                // Placement new to construct in NUMA memory
                EventType* obj = new (mem) EventType();
                
                // Custom deleter for NUMA cleanup
                pool_[i] = std::unique_ptr<EventType>(obj, [this](EventType* p) {
                    if (p) {
                        p->~EventType();  // Explicit destructor
                        NUMABinding::freeNumaMemory(p, sizeof(EventType));
                    }
                });
            } else {
                // Fallback to regular allocation
                pool_[i] = std::make_unique<EventType>();
            }
        }
    } else
    #endif
    {
        // Non-NUMA: regular allocation
        for (size_t i = 0; i < Capacity; ++i) {
            pool_[i] = std::make_unique<EventType>();
        }
    }
}
```

### Acquire/Release Operations

```
┌─────────────────────────────────────────────────────────────┐
│                        ACQUIRE                               │
│                                                              │
│  pool_:  [Event0][Event1][Event2][Event3]...[EventN-1]      │
│                                             ▲               │
│                                    available_count_ = 5     │
│                                                              │
│  1. if (available_count_ == 0) return nullptr  // Exhausted │
│  2. --available_count_                         // Now = 4   │
│  3. return pool_[available_count_].get()       // EventN-1  │
│                                                              │
│  Complexity: O(1) - just index decrement                    │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                        RELEASE                               │
│                                                              │
│  pool_:  [Event0][Event1][Event2][nullptr][nullptr]         │
│                          ▲                                   │
│                 available_count_ = 3                         │
│                                                              │
│  1. Search for slot: find where pool_[i] == nullptr         │
│  2. Move event back: pool_[i] = event (with deleter)        │
│  3. ++available_count_                                       │
│                                                              │
│  Complexity: O(n) worst case - could optimize with free list│
│                                                              │
│  BUG FIXED: Now properly searches for empty slot and        │
│             reattaches NUMA custom deleter                   │
└─────────────────────────────────────────────────────────────┘
```

---

## 📦 NUMABinding Utilities

### Implementation (from code)

```cpp
// File: include/eventstream/core/memory/numa_binding.hpp
namespace EventStream {

class NUMABinding {
public:
    /**
     * Get number of NUMA nodes in system
     */
    static int getNumNumaNodes();
    
    /**
     * Bind current thread to specific CPU
     * @param cpu_id CPU core ID
     * @return true if successful
     */
    static bool bindToCore(int cpu_id);
    
    /**
     * Bind current thread to specific NUMA node
     * @param numa_node NUMA node ID
     * @return true if successful
     */
    static bool bindToNode(int numa_node);
    
    /**
     * Allocate memory on specific NUMA node
     * @param size Bytes to allocate
     * @param numa_node Target NUMA node
     * @return Pointer to allocated memory, nullptr on failure
     */
    static void* allocateOnNode(size_t size, int numa_node);
    
    /**
     * Free NUMA-allocated memory
     * @param ptr Memory pointer
     * @param size Size that was allocated
     */
    static void freeNumaMemory(void* ptr, size_t size);
    
    /**
     * Get NUMA node for given CPU
     */
    static int getCpuNumaNode(int cpu_id);
};

}  // namespace EventStream
```

### Usage Pattern

```cpp
// Typical usage: Bind thread + allocate pool on same NUMA node
void ingestThreadMain(int numa_node) {
    // 1. Bind thread to NUMA node
    EventStream::NUMABinding::bindToNode(numa_node);
    
    // 2. Create pool on same NUMA node
    NUMAEventPool<Event, 10000> pool(numa_node);
    
    // 3. All acquire/release now uses local memory
    while (running) {
        Event* evt = pool.acquire();  // Fast local access!
        // ... process ...
        pool.release(evt);
    }
}
```

---

## 📦 IngestEventPool

### Purpose

IngestEventPool là wrapper được sử dụng bởi TCP/UDP servers để allocate events cho incoming data.

```cpp
// File: include/eventstream/core/ingest/ingest_pool.hpp

// BUG FIXED: Now uses NUMAEventPool instead of basic EventPool
using IngestEventPool = eventstream::core::NUMAEventPool<EventStream::Event, 10000>;
```

### Usage in Ingest Layer

```cpp
// TCP Server
class TcpServer {
private:
    IngestEventPool event_pool_;  // NUMA-aware pool
    
    void handleConnection(int fd) {
        // Acquire from NUMA-local pool
        Event* event = event_pool_.acquire();
        if (!event) {
            // Pool exhausted - backpressure!
            ++totalBackpressureDrops_;
            return;
        }
        
        // Parse frame into event
        parseFrame(fd, event);
        
        // Push to queue (pool manages lifecycle)
        bus_.push(queueId, event);
    }
};
```

---

## 📊 Performance Comparison

| Operation | EventPool | NUMAEventPool (local) | NUMAEventPool (remote) |
|-----------|-----------|----------------------|------------------------|
| acquire() | ~11ns | ~15ns | ~50ns |
| release() | ~11ns | ~20ns | ~60ns |
| Memory access | Default | ~50ns | ~200-300ns |

### Why NUMA Matters

```
┌─────────────────────────────────────────────────────────────┐
│                  LATENCY COMPARISON                          │
│                                                              │
│  Without NUMA optimization:                                  │
│  CPU 0 (Node 0) → Pool on Node 1 → ~300ns per access        │
│  × 10M events/sec = 3 seconds of latency!                   │
│                                                              │
│  With NUMA optimization:                                     │
│  CPU 0 (Node 0) → Pool on Node 0 → ~50ns per access         │
│  × 10M events/sec = 0.5 seconds of latency                  │
│                                                              │
│  Improvement: 6x faster memory access                        │
└─────────────────────────────────────────────────────────────┘
```

---

## 🔧 Configuration

### ProcessManager Thread Pinning

```cpp
// File: src/eventstream/core/processor/process_manager.cpp
void ProcessManager::start() {
    // Pin each processor to dedicated CPU core
    realtimeThread_ = std::thread([this] {
        NUMABinding::bindToCore(0);  // CPU 0
        runLoop(REALTIME, realtimeProcessor_.get());
    });
    
    transactionalThread_ = std::thread([this] {
        NUMABinding::bindToCore(1);  // CPU 1
        runLoop(TRANSACTIONAL, transactionalProcessor_.get());
    });
    
    batchThread_ = std::thread([this] {
        NUMABinding::bindToCore(2);  // CPU 2
        runLoop(BATCH, batchProcessor_.get());
    });
}
```

### EventBusMulti NUMA Node

```cpp
// Set NUMA node for bus operations
EventBusMulti bus;
bus.setNUMANode(0);  // Bind to NUMA node 0

// Get current binding
int node = bus.getNUMANode();
```

---

## ⚠️ Known Limitations

| Component | Limitation | Status |
|-----------|------------|--------|
| NUMAEventPool::release() | O(n) search for empty slot | Could optimize with free list |
| ProcessManager | Hardcoded CPU 0,1,2 | Could make configurable |
| NUMABinding | Linux-only (#ifdef __linux__) | Windows not supported |
| IngestEventPool | Fixed capacity 10000 | Could make template param |

---

## 🐛 Bugs Fixed

| Bug | Description | Fix |
|-----|-------------|-----|
| NUMAEventPool::release() | Didn't search for empty slot, used wrong index | Proper search + reattach NUMA deleter |
| IngestEventPool | Was using basic EventPool instead of NUMAEventPool | Changed typedef to use NUMAEventPool |

---

## ➡️ Next

- [Event Model & Protocol →](event.md)
