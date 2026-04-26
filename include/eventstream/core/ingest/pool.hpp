#pragma once

#include <eventstream/core/events/event.hpp>
#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <spdlog/spdlog.h>

namespace EventStream {

/**
 * @brief Thread-safe global event pool for ingestion.
 *
 * Pre-allocates events and hands out shared_ptr with a custom deleter
 * that returns events to the pool when the refcount drops to zero.
 * Safe for events that cross thread boundaries (TCP -> Dispatcher -> Processor).
 *
 * For single-thread benchmarking, use EventPool instead.
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
        
        // Mark pool as active (not shutting down)
        getShutdownFlag().store(false, std::memory_order_release);
        
        // Pre-allocate all events
        for (size_t i = 0; i < kPoolCapacity; ++i) {
            pool.push(std::unique_ptr<Event>(new Event()));
        }
        spdlog::info("[IngestEventPool] Initialized with {} pre-allocated events", kPoolCapacity);
    }

    /**
     * @brief Shutdown pool - must be called before process exit
     * Prevents custom deleters from returning events to destroyed pool
     */
    static void shutdown() {
        // Mark as shutting down BEFORE acquiring lock
        // This prevents in-flight deleters from trying to return to pool
        getShutdownFlag().store(true, std::memory_order_release);
        
        // Clear pool
        {
            std::lock_guard<std::mutex> lock(getPoolMutex());
            auto& pool = getPool();
            while (!pool.empty()) {
                pool.pop();
            }
        }
        spdlog::info("[IngestEventPool] Shutdown complete");
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
        // Captures shutdown flag by reference for safe shutdown handling
        return std::shared_ptr<Event>(evt.release(), [](Event* e) {
            if (!e) return;
            
            // Check if pool is shutting down - if so, just delete
            if (getShutdownFlag().load(std::memory_order_acquire)) {
                delete e;
                return;
            }
            
            // Reset event to clean state via move-assignment.
            *e = Event{};
            
            // Return to pool with proper locking
            {
                std::lock_guard<std::mutex> lock(getPoolMutex());
                
                // Double-check shutdown flag while holding lock
                if (getShutdownFlag().load(std::memory_order_acquire)) {
                    delete e;
                    return;
                }
                
                auto& pool = getPool();
                if (pool.size() < kPoolCapacity) {
                    pool.push(std::unique_ptr<Event>(e));
                } else {
                    // Pool full, just delete
                    delete e;
                }
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
    
    static std::atomic<bool>& getShutdownFlag() {
        static std::atomic<bool> shutdown_flag{false};
        return shutdown_flag;
    }
};

}  // namespace EventStream

