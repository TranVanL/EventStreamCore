#pragma once

#include "core/memory/event_pool.hpp"
#include "event/Event.hpp"
#include <memory>
#include <thread>
#include <unordered_map>
#include <mutex>

namespace EventStream {

/**
 * TCP Event Pool Manager
 * 
 * Maintains per-thread event pools for TCP ingest threads
 * Eliminates allocation overhead for high-frequency event creation
 * 
 * Usage:
 *   - Each TCP client handler thread gets its own pool
 *   - Pool capacity = max events in flight per thread
 *   - Events are shared_ptr wrappers around pooled Event objects
 */
class TcpEventPoolManager {
public:
    static constexpr size_t EVENTS_PER_THREAD = 1000;  // Max concurrent events per client
    
    /**
     * Get thread-local event pool
     * Creates one if it doesn't exist for current thread
     */
    static std::shared_ptr<EventStream::Event> acquireEvent() {
        auto& pool = getThreadPool();
        
        // Get raw event from pool
        Event* evt = pool.acquire();
        if (!evt) {
            // Fallback - shouldn't happen if capacity is correct
            evt = new Event();
        }
        
        // Wrap in shared_ptr with custom deleter to return to pool
        return std::shared_ptr<Event>(evt, [](Event* e) {
            getThreadPool().release(e);
        });
    }
    
    /**
     * Reset all thread pools (for shutdown/cleanup)
     */
    static void resetAll() {
        std::lock_guard<std::mutex> lock(instance_mutex_);
        pools_.clear();
    }
    
private:
    using EventPoolType = eventstream::core::EventPool<Event, EVENTS_PER_THREAD>;
    
    static EventPoolType& getThreadPool() {
        thread_local static EventPoolType pool;
        return pool;
    }
    
    static std::mutex instance_mutex_;
    static std::unordered_map<std::thread::id, EventPoolType*> pools_;
};

}  // namespace EventStream
