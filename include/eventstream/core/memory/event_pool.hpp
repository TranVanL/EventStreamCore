#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <cassert>
#include <queue>
#include <mutex>
#include <unordered_map>

namespace eventstream::core {

/**
 * PooledEvent - Base class for events that can be pooled
 * Contains intrusive pool index for O(1) release
 */
template<typename Derived>
struct PooledEvent {
    // Index in the pool for O(1) release lookup
    // Set by EventPool on acquire, used on release
    size_t pool_index_{SIZE_MAX};
    
    // Check if this event was acquired from a pool
    bool isPooled() const { return pool_index_ != SIZE_MAX; }
};

/**
 * EventPool - Per-thread event object reuse pool with STATIC ALLOCATION
 * 
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  ⚠️  BENCHMARK / SINGLE-THREAD USE ONLY                            │
 * │                                                                     │
 * │  For production multi-threaded code, use IngestEventPool instead   │
 * │  which provides thread-safe shared_ptr with automatic lifecycle    │
 * │  management.                                                       │
 * └─────────────────────────────────────────────────────────────────────┘
 * 
 * Eliminates repeated allocation/deallocation of events
 * by recycling objects through acquire/release pattern.
 * 
 * THREAD-SAFETY: NONE - One instance per producer thread only!
 * No locks needed because each thread owns its pool exclusively.
 * 
 * Design:
 * - Static array instead of vector (no capacity management overhead)
 * - O(1) acquire and release using intrusive index
 * - Zero allocation in fast path
 * 
 * Requirements:
 * - EventType should inherit from PooledEvent<EventType> for O(1) release
 * - If not, falls back to hash map lookup (still fast)
 * 
 * Benefits:
 * - O(1) acquire and release
 * - No allocator contention across threads
 * - Stable latency (no GC pauses or vector reallocation)
 * - Cache-friendly (events pre-allocated)
 * 
 * Usage (BENCHMARK ONLY):
 *   struct MyEvent : PooledEvent<MyEvent> {
 *       int data;
 *   };
 *   EventPool<MyEvent, 10000> pool;  // Per-thread pool
 *   
 *   // Same thread only!
 *   MyEvent* evt = pool.acquire();
 *   evt->data = ...;
 *   // ... use event ...
 *   pool.release(evt);  // O(1) return to pool
 */
template<typename EventType, size_t Capacity>
class EventPool {
    // Static storage for all events
    std::array<std::unique_ptr<EventType>, Capacity> pool_;
    
    // Hash map from event pointer to pool index for non-intrusive types
    // Only used if EventType doesn't have pool_index_ field
    std::unordered_map<EventType*, size_t> ptr_to_index_;
    
    // Free list: indices of available slots in pool_
    // free_list_[0..available_count_-1] contains indices of free slots
    std::array<size_t, Capacity> free_list_;
    
    // Number of available events (from 0 to Capacity)
    size_t available_count_;
    
    // Helper to check if EventType has pool_index_ (SFINAE)
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
    
public:
    /**
     * Create event pool with static capacity
     * All events pre-allocated in constructor
     */
    explicit EventPool()
        : available_count_(Capacity) {
        
        // Pre-allocate all events and initialize free list
        for (size_t i = 0; i < Capacity; ++i) {
            pool_[i] = std::make_unique<EventType>();
            free_list_[i] = i;  // All slots initially free
            
            // Set intrusive index if supported
            if constexpr (kHasPoolIndex) {
                pool_[i]->pool_index_ = i;
            } else {
                // Use hash map for non-intrusive types
                ptr_to_index_[pool_[i].get()] = i;
            }
        }
    }
    
    /**
     * Destructor - all events automatically freed via unique_ptr
     */
    ~EventPool() = default;
    
    /**
     * Acquire event from pool
     * O(1) operation - just decrement counter and return pointer
     * 
     * @return Pointer to event object ready for use
     * 
     * NOTE: If pool is exhausted, allocates from heap (fallback)
     */
    EventType* acquire() {
        if (available_count_ == 0) {
            // Fallback for release builds - allocate from heap
            // Mark as non-pooled (pool_index_ remains SIZE_MAX)
            return new EventType();
        }
        
        available_count_--;
        // Get index from free list and return that event
        size_t idx = free_list_[available_count_];
        return pool_[idx].get();
    }
    
    /**
     * Release event back to pool for reuse
     * 
     * COMPLEXITY: O(1) with intrusive index, O(1) amortized with hash map
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
        
        if (available_count_ >= Capacity) {
            // Pool full - this might be a heap-allocated fallback event
            delete obj;
            return;
        }
        
        size_t idx = SIZE_MAX;
        
        // O(1) lookup using intrusive index
        if constexpr (kHasPoolIndex) {
            idx = obj->pool_index_;
            if (idx == SIZE_MAX) {
                // Heap-allocated fallback event
                delete obj;
                return;
            }
        } else {
            // O(1) amortized lookup using hash map
            auto it = ptr_to_index_.find(obj);
            if (it == ptr_to_index_.end()) {
                // Object not in pool - heap allocated fallback
                delete obj;
                return;
            }
            idx = it->second;
        }
        
        // Validate index is within bounds
        if (idx >= Capacity) {
            delete obj;
            return;
        }
        
        free_list_[available_count_] = idx;
        available_count_++;
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
        for (size_t i = 0; i < Capacity; ++i) {
            pool_[i] = std::make_unique<EventType>();
            free_list_[i] = i;
            
            // Reset intrusive index or hash map
            if constexpr (kHasPoolIndex) {
                pool_[i]->pool_index_ = i;
            } else {
                ptr_to_index_[pool_[i].get()] = i;
            }
        }
    }
};

} // namespace eventstream::core
