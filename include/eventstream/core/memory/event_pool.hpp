#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <cassert>

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
    
    // Number of available events (from 0 to Capacity)
    size_t available_count_;
    
public:
    /**
     * Create event pool with static capacity
     * All events pre-allocated in constructor
     */
    explicit EventPool()
        : available_count_(Capacity) {
        
        // Pre-allocate all events
        for (size_t i = 0; i < Capacity; ++i) {
            pool_[i] = std::make_unique<EventType>();
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
        // Return raw pointer, but keep it in unique_ptr for memory safety
        return pool_[available_count_].get();
    }
    
    /**
     * Release event back to pool for reuse
     * O(1) operation - just increment counter
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
        
        if (available_count_ < Capacity) {
            // Don't create new unique_ptr, it's already managed
            available_count_++;
        }
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
        }
    }
};

} // namespace eventstream::core
