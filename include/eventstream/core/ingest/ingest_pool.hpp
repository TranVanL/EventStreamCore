#pragma once

#include <eventstream/core/memory/event_pool.hpp>
#include <eventstream/core/memory/numa_event_pool.hpp>
#include <eventstream/core/memory/numa_binding.hpp>
#include <eventstream/core/events/event.hpp>
#include <memory>
#include <thread>
#include <mutex>
#include <spdlog/spdlog.h>

namespace EventStream {

/**
 * @brief Ingest Event Pool Manager
 * 
 * Maintains per-thread event pools for all ingest threads (TCP, UDP, etc.)
 * Eliminates allocation overhead for high-frequency event creation.
 * 
 * Architecture:
 * - Thread-local pools: Each ingest thread (TCP client handler, UDP receiver) gets its own pool
 * - Pool capacity: Max events in flight per thread
 * - Zero-copy: Events are shared_ptr wrappers with custom deleters
 * - NUMA-aware: Allocate pool memory on same node as thread for locality
 * 
 * Usage:
 *   // In TCP/UDP ingest thread initialization
 *   IngestEventPool::bindToNUMA(numa_node);
 * 
 *   // Acquire event (zero malloc overhead after warmup)
 *   auto event = IngestEventPool::acquireEvent();
 */
class IngestEventPool {
public:
    static constexpr size_t kEventsPerThread = 1000;  // Max concurrent events per ingest thread

    /**
     * @brief Acquire event from thread-local pool
     * @return Shared pointer to pooled event with custom deleter
     * 
     * The returned shared_ptr automatically returns the event to pool when ref count hits zero.
     */
    static std::shared_ptr<Event> acquireEvent() {
        auto& pool = getThreadPool();

        // Acquire raw event from pool
        Event* evt = pool.acquire();
        if (!evt) {
            // Fallback allocation if pool exhausted (shouldn't happen with proper sizing)
            spdlog::warn("[IngestEventPool] Pool exhausted, falling back to heap allocation");
            evt = new Event();
        }

        // Wrap in shared_ptr with custom deleter to return to pool
        return std::shared_ptr<Event>(evt, [](Event* e) {
            auto& pool = getThreadPool();
            pool.release(e);
        });
    }

    /**
     * @brief Bind current ingest thread to NUMA node and initialize pool
     * @param numaNode NUMA node ID (-1 to skip binding)
     * 
     * Call this at the start of each ingest thread (TCP/UDP handler).
     * Pool is allocated on the specified NUMA node for optimal memory locality.
     */
    static void bindToNUMA(int numaNode) {
        // Store NUMA node for pool initialization
        getNumaNode() = numaNode;
        
        if (numaNode >= 0) {
            int cpu = EventStream::NUMABinding::bindThreadToNUMANode(numaNode);
            if (cpu >= 0) {
                spdlog::debug("[IngestEventPool] Ingest thread bound to NUMA node {} (CPU {})",
                              numaNode, cpu);
            }
        }
        // Initialize thread-local pool (will use stored NUMA node)
        getThreadPool();
    }

private:
    // Use NUMAEventPool for NUMA-aware memory allocation
    using EventPoolType = eventstream::core::NUMAEventPool<Event, kEventsPerThread>;

    static int& getNumaNode() {
        thread_local static int numa_node = -1;
        return numa_node;
    }

    static EventPoolType& getThreadPool() {
        // Create pool with NUMA node binding
        thread_local static EventPoolType pool(getNumaNode());
        return pool;
    }
};

}  // namespace EventStream
