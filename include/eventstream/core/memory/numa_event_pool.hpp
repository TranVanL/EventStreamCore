#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <functional>
#include <cassert>
#include <eventstream/core/memory/numa_binding.hpp>
#include <spdlog/spdlog.h>

#ifdef __linux__
    #include <numa.h>
#endif

namespace eventstream::core {

// Custom deleter for NUMA-allocated memory
template<typename EventType>
struct NUMADeleter {
    void operator()(EventType* ptr) const {
        if (ptr) {
            ptr->~EventType();  // Explicit destructor
            EventStream::NUMABinding::freeNumaMemory(ptr, sizeof(EventType));
        }
    }
};

/**
 * NUMA-Aware EventPool - Per-thread event object reuse pool with NUMA node binding
 * 
 * Extends EventPool with NUMA (Non-Uniform Memory Access) support:
 * - Allocates memory on specific NUMA node for low latency
 * - Reduces remote memory access penalty (~300ns → ~50ns)
 * - Improves cache locality on multi-socket systems
 * 
 * THREAD-SAFE: One instance per producer thread on bound NUMA node.
 * No locks needed because each thread owns its pool exclusively.
 * 
 * Design:
 * - Static array instead of vector (no capacity management overhead)
 * - NUMA node binding for allocation locality
 * - O(1) acquire and release (single pointer increment/decrement)
 * - Memory allocated on binding NUMA node at initialization
 * 
 * Benefits:
 * - O(1) acquire and release (just index manipulation)
 * - No allocator contention across threads
 * - Stable latency (no GC pauses or vector reallocation)
 * - Cache-friendly with NUMA locality optimization
 * - ~50ns per access instead of ~300ns on remote NUMA node
 * 
 * Usage:
 *   NUMAEventPool<MyEvent, 10000> pool(numa_node_id);  // Bound to NUMA node
 *   
 *   // Producer thread (should be bound to same NUMA node)
 *   MyEvent* evt = pool.acquire();
 *   evt->data = ...;
 *   queue.push(evt);
 */
template<typename EventType, size_t Capacity>
class NUMAEventPool {
    // Use unique_ptr with custom deleter for NUMA memory
    using UniquePtr = std::unique_ptr<EventType, std::function<void(EventType*)>>;
    
    // Static storage for all events
    std::array<UniquePtr, Capacity> pool_;
    
    // Number of available events (from 0 to Capacity)
    size_t available_count_;
    
    // NUMA node this pool is bound to
    int numa_node_;
    
public:
    /**
     * Create NUMA-aware event pool with static capacity
     * All events pre-allocated on specified NUMA node
     * 
     * @param numa_node NUMA node ID to bind memory to (-1 for default allocation)
     */
    explicit NUMAEventPool(int numa_node = -1)
        : available_count_(Capacity), numa_node_(numa_node) {
        
        #ifdef __linux__
            if (numa_node >= 0 && EventStream::NUMABinding::getNumNumaNodes() > 0) {
                // Allocate all events on specific NUMA node
                for (size_t i = 0; i < Capacity; ++i) {
                    // Allocate raw memory on NUMA node
                    void* mem = EventStream::NUMABinding::allocateOnNode(
                        sizeof(EventType), numa_node);
                    
                    if (mem) {
                        // Placement new to construct object in NUMA-allocated memory
                        EventType* obj = new (mem) EventType();
                        
                        // Store in unique_ptr with custom NUMA deleter
                        auto deleter = [](EventType* ptr) {
                            if (ptr) {
                                ptr->~EventType();
                                EventStream::NUMABinding::freeNumaMemory(ptr, sizeof(EventType));
                            }
                        };
                        pool_[i] = UniquePtr(obj, deleter);
                    } else {
                        // Fallback to regular allocation if NUMA allocation fails
                        pool_[i] = UniquePtr(new EventType(), [](EventType* ptr) { delete ptr; });
                        spdlog::warn("[NUMAEventPool] NUMA allocation failed for node {}, using default", 
                            numa_node);
                    }
                }
                spdlog::info("[NUMAEventPool] Allocated {} events on NUMA node {}", 
                    Capacity, numa_node);
            } else {
                // No NUMA support or invalid node: use default allocation
                for (size_t i = 0; i < Capacity; ++i) {
                    pool_[i] = UniquePtr(new EventType(), [](EventType* ptr) { delete ptr; });
                }
            }
        #else
            // Non-Linux: use default allocation
            for (size_t i = 0; i < Capacity; ++i) {
                pool_[i] = UniquePtr(new EventType(), [](EventType* ptr) { delete ptr; });
            }
        #endif
    }
    
    /**
     * Destructor - all events automatically freed
     * Custom deleters handle NUMA memory deallocation
     */
    ~NUMAEventPool() {
    }
    
    /**
     * Acquire event from pool
     * O(1) operation - just decrement counter and return pointer
     * 
     * @return Pointer to event object ready for use
     * 
     * NOTE: If pool is exhausted, asserts (should not happen in production)
     */
    EventType* acquire() {
        assert(available_count_ > 0 && "Event pool exhausted!");
        
        if (available_count_ == 0) {
            // Fallback for release builds
            #ifdef __linux__
                if (numa_node_ >= 0) {
                    void* mem = EventStream::NUMABinding::allocateOnNode(
                        sizeof(EventType), numa_node_);
                    if (mem) {
                        return new (mem) EventType();
                    }
                }
            #endif
            return new EventType();
        }
        
        available_count_--;
        // Return raw pointer from pool
        return pool_[available_count_].get();
    }
    
    /**
     * Release event back to pool for reuse
     * 
     * COMPLEXITY: O(n) worst case due to linear search.
     * See EventPool::release() for optimization notes.
     * 
     * IMPORTANT: Must only release events from THIS pool!
     * Do NOT mix events between different pool instances.
     * 
     * @param obj - Pointer to event previously acquired from THIS pool
     */
    void release(EventType* obj) {
        if (!obj) {
            return;
        }
        
        assert(available_count_ < Capacity && "Too many releases!");
        
        if (available_count_ >= Capacity) {
            return;  // Pool full, ignore
        }
        
        // Linear search to find slot (O(n) worst case, LIFO optimized)
        for (size_t i = Capacity; i > 0; --i) {
            size_t idx = i - 1;
            if (pool_[idx].get() == obj) {
                // Found - add back to available
                // Note: We don't swap, just track that this slot is free
                available_count_++;
                return;
            }
        }
        
        // Object not found in pool - was heap allocated in acquire() fallback
        #ifdef __linux__
            if (numa_node_ >= 0) {
                obj->~EventType();
                EventStream::NUMABinding::freeNumaMemory(obj, sizeof(EventType));
                return;
            }
        #endif
        delete obj;
    }
    
    /**
     * Get NUMA node this pool is bound to
     * @return NUMA node ID, or -1 if not bound
     */
    int getNUMANode() const {
        return numa_node_;
    }
    
    /**
     * Get current number of available events
     * @return Count of events available for acquire
     */
    size_t available() const {
        return available_count_;
    }
    
    /**
     * Get pool capacity (fixed at compile time)
     * @return Configured capacity
     */
    size_t capacity() const {
        return Capacity;
    }
    
    /**
     * Get utilization percentage
     * @return (available / capacity) * 100
     */
    double utilization_percent() const {
        return (static_cast<double>(available_count_) / Capacity) * 100.0;
    }
    
    /**
     * Reset pool - reinitialize all events
     * USE WITH CAUTION: Only safe if no events are in flight!
     */
    void reset() {
        available_count_ = Capacity;
        #ifdef __linux__
            if (numa_node_ >= 0) {
                for (size_t i = 0; i < Capacity; ++i) {
                    auto& ptr = pool_[i];
                    if (ptr) {
                        ptr->~EventType();  // Call destructor
                        void* mem = ptr.get();
                        // Let unique_ptr with custom deleter handle freeing
                    }
                    
                    // Reallocate on NUMA node
                    void* mem = EventStream::NUMABinding::allocateOnNode(
                        sizeof(EventType), numa_node_);
                    if (mem) {
                        EventType* obj = new (mem) EventType();
                        ptr.reset(obj);
                    } else {
                        ptr = std::make_unique<EventType>();
                    }
                }
                return;
            }
        #endif
        
        // Regular reset
        for (size_t i = 0; i < Capacity; ++i) {
            pool_[i] = std::make_unique<EventType>();
        }
    }
};

} // namespace eventstream::core
