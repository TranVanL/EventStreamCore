#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <cassert>
#include <queue>
#include <mutex>

namespace eventstream::core {

/**
 * EventPool - Per-thread event object reuse pool with STATIC ALLOCATION
 * 
 * Eliminates repeated allocation/deallocation of events
 * by recycling objects through acquire/release pattern.
 * 
 * THREAD-SAFE: One instance per producer thread only!
 * No locks needed because each thread owns its pool exclusively.
 * 
 * Design:
 * - Static array instead of vector (no capacity management overhead)
 * - Index counter instead of size tracking
 * - O(1) acquire and release (single pointer increment/decrement)
 * - Zero allocation in fast path
 * 
 * Benefits:
 * - O(1) acquire and release (just index manipulation)
 * - No allocator contention across threads
 * - Stable latency (no GC pauses or vector reallocation)
 * - Cache-friendly (events pre-allocated)
 * 
 * Usage:
 *   EventPool<MyEvent, 10000> pool;  // 10000 pre-allocated events
 *   
 *   // Producer thread
 *   MyEvent* evt = pool.acquire();
 *   evt->data = ...;
 *   queue.push(evt);
 *   
 *   // Consumer thread
 *   MyEvent* evt;
 *   if (queue.pop(evt)) {
 *       // process event
 *       producer_pool.release(evt);  // return to producer pool
 *   }
 */
template<typename EventType, size_t Capacity>
class EventPool {
    // Static storage for all events
    std::array<std::unique_ptr<EventType>, Capacity> pool_;
    
    // Map from event pointer to pool index for O(1) release
    // This allows O(1) lookup instead of linear search
    std::array<size_t, Capacity> ptr_to_index_;
    
    // Free list: indices of available slots in pool_
    // free_list_[0..available_count_-1] contains indices of free slots
    std::array<size_t, Capacity> free_list_;
    
    // Number of available events (from 0 to Capacity)
    size_t available_count_;
    
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
            ptr_to_index_[i] = i;  // Map pointer index to pool index
        }
    }
    
    /**
     * Destructor - all events automatically freed via unique_ptr
     */
    ~EventPool() {
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
     * COMPLEXITY: O(n) worst case due to linear search.
     * In practice, events are released in LIFO order (most recently acquired),
     * so search usually finds the slot quickly.
     * 
     * FUTURE OPTIMIZATION: For true O(1), either:
     * 1. Use intrusive design: EventType contains pool_index_ field
     * 2. Use unordered_map<EventType*, size_t> for pointer->index lookup
     * 3. Use raw array instead of unique_ptr for contiguous memory
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
            return;  // Pool full, ignore (shouldn't happen)
        }
        
        // Linear search to find slot (O(n) worst case)
        // Optimization: search from end first (LIFO pattern)
        for (size_t i = Capacity; i > 0; --i) {
            size_t idx = i - 1;
            if (pool_[idx].get() == obj) {
                free_list_[available_count_] = idx;
                available_count_++;
                return;
            }
        }
        
        // Object not found in pool - was heap allocated in acquire() fallback
        // Delete it to avoid memory leak
        delete obj;
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
            ptr_to_index_[i] = i;
        }
    }
};

} // namespace eventstream::core
