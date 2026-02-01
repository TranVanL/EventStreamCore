#pragma once

#include <eventstream/core/events/event.hpp>
#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <spdlog/spdlog.h>

namespace EventStream {

/**
 * @brief Thread-Safe Global Event Pool for Ingestion
 * 
 * Pre-allocates events to avoid allocation overhead.
 * Uses a thread-safe queue so events can be acquired in one thread
 * and deleted in another thread safely.
 * 
 * Architecture:
 * - Shared pool with mutex protection
 * - Events are returned to pool when shared_ptr refcount hits zero
 * - Safe for events crossing thread boundaries
 * - Pre-allocates on initialization for predictable latency
 */
class IngestEventPool {
public:
    static constexpr size_t kPoolCapacity = 10000;  // Pre-allocated events

    /**
     * @brief Initialize pool with pre-allocated events
     * Call this once at startup
     */
    static void initialize() {
        auto& pool = getPool();
        std::lock_guard<std::mutex> lock(getPoolMutex());
        
        // Pre-allocate all events
        for (size_t i = 0; i < kPoolCapacity; ++i) {
            pool.push(std::unique_ptr<Event>(new Event()));
        }
        spdlog::info("[IngestEventPool] Initialized with {} pre-allocated events", kPoolCapacity);
    }

    /**
     * @brief Acquire event from thread-safe pool
     * @return Shared pointer to event with custom deleter to return to pool
     */
    static std::shared_ptr<Event> acquireEvent() {
        std::unique_ptr<Event> evt;
        
        {
            std::lock_guard<std::mutex> lock(getPoolMutex());
            auto& pool = getPool();
            
            if (!pool.empty()) {
                evt = std::move(pool.front());
                pool.pop();
            }
        }
        
        if (!evt) {
            // Fallback: allocate new event if pool exhausted
            spdlog::warn("[IngestEventPool] Pool exhausted, allocating new event from heap");
            evt = std::unique_ptr<Event>(new Event());
        }

        // Wrap in shared_ptr with custom deleter to return to pool
        return std::shared_ptr<Event>(evt.release(), [](Event* e) {
            if (!e) return;
            
            // Reset event to clean state
            e->~Event();
            new (e) Event();  // Placement new to reinitialize
            
            // Return to pool
            std::lock_guard<std::mutex> lock(getPoolMutex());
            auto& pool = getPool();
            
            if (pool.size() < kPoolCapacity) {
                pool.push(std::unique_ptr<Event>(e));
            } else {
                // Pool full, just delete
                delete e;
            }
        });
    }

    /**
     * @brief Bind ingest thread to NUMA node
     * @param numaNode NUMA node ID (-1 to skip)
     */
    static void bindToNUMA(int numaNode) {
        // Thread affinity binding could go here if needed
        (void)numaNode;
    }

    /**
     * @brief Get current pool size (for monitoring)
     */
    static size_t getPoolSize() {
        std::lock_guard<std::mutex> lock(getPoolMutex());
        return getPool().size();
    }

private:
    static std::queue<std::unique_ptr<Event>>& getPool() {
        static std::queue<std::unique_ptr<Event>> pool;
        return pool;
    }

    static std::mutex& getPoolMutex() {
        static std::mutex mtx;
        return mtx;
    }
};

}  // namespace EventStream

