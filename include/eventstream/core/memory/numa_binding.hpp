#pragma once

#include <cstdint>
#include <thread>
#include <vector>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>

#ifdef __linux__
    #include <sched.h>
    #include <numa.h>
    #include <numaif.h>
#endif

namespace EventStream {

/**
 * @brief NUMA (Non-Uniform Memory Access) binding utilities
 * 
 * Provides thread CPU affinity and memory allocation binding to NUMA nodes.
 * This reduces memory access latency and improves cache locality on NUMA systems.
 * 
 * Key optimizations:
 * - Bind producer/consumer threads to specific CPUs to maximize cache reuse
 * - Allocate memory on the same NUMA node as processing threads
 * - Reduce remote memory access (high latency ~300ns vs local ~50ns)
 */
class NUMABinding {
public:
    /**
     * @brief Get available NUMA nodes on system
     * @return Number of NUMA nodes, 1 if NUMA not available
     */
    static int getNumNumaNodes() {
        #ifdef __linux__
            if (numa_available() != -1) {
                return numa_max_node() + 1;
            }
        #endif
        return 1;
    }

    /**
     * @brief Get number of CPUs on specific NUMA node
     * @param numa_node NUMA node ID
     * @return Number of CPUs on node
     */
    static int getCPUCountOnNode(int numa_node) {
        #ifdef __linux__
            int max_nodes = getNumNumaNodes();
            if (numa_available() != -1 && numa_node >= 0 && numa_node < max_nodes) {
                struct bitmask* cpumask = numa_allocate_cpumask();
                if (cpumask) {
                    numa_node_to_cpus(numa_node, cpumask);
                    int count = numa_bitmask_weight(cpumask);
                    numa_free_cpumask(cpumask);
                    return count;
                }
            }
        #endif
        return 0;
    }

    /**
     * @brief Bind current thread to specific CPU
     * @param cpu_id CPU ID to bind to
     * @return true if successful, false otherwise
     */
    static bool bindThreadToCPU(int cpu_id) {
        #ifdef __linux__
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_id, &cpuset);
            
            if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
                spdlog::debug("[NUMA] Thread bound to CPU {}", cpu_id);
                return true;
            } else {
                spdlog::warn("[NUMA] Failed to bind thread to CPU {}", cpu_id);
                return false;
            }
        #else
            spdlog::debug("[NUMA] CPU binding not supported on this platform");
            return false;
        #endif
    }

    /**
     * @brief Bind a std::thread object to specific CPU
     * @param t Thread object to bind (must be joinable)
     * @param cpu_id CPU ID to bind to
     * @return true if successful, false otherwise
     * @throws std::runtime_error if thread is not joinable
     */
    static bool bindThread(std::thread& t, int cpu_id) {
        if (!t.joinable()) {
            throw std::runtime_error("Thread is not joinable - cannot set affinity");
        }
        
        #ifdef __linux__
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_id, &cpuset);
            int rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
            if (rc == 0) {
                spdlog::debug("[NUMA] Thread object bound to CPU {}", cpu_id);
                return true;
            } else {
                spdlog::warn("[NUMA] Failed to bind thread object to CPU {}: {}", cpu_id, rc);
                return false;
            }
        #elif _WIN32
            if (cpu_id < 0 || cpu_id >= 64) {
                throw std::runtime_error("Invalid cpu_id: " + std::to_string(cpu_id));
            }
            DWORD_PTR mask = 1ULL << cpu_id;
            HANDLE threadHandle = (HANDLE)t.native_handle();
            if (threadHandle != NULL && threadHandle != INVALID_HANDLE_VALUE) {
                DWORD_PTR result = SetThreadAffinityMask(threadHandle, mask);
                if (result != 0) {
                    spdlog::debug("[NUMA] Thread object bound to CPU {}", cpu_id);
                    return true;
                } else {
                    spdlog::warn("[NUMA] Failed to bind thread object to CPU {}", cpu_id);
                    return false;
                }
            }
            return false;
        #else
            throw std::runtime_error("Thread affinity not supported on this platform");
        #endif
    }

    /**
     * @brief Bind current thread to NUMA node (uses first available CPU)
     * @param numa_node NUMA node ID to bind to
     * @return CPU ID bound to, -1 if failed
     */
    static int bindThreadToNUMANode(int numa_node) {
        #ifdef __linux__
            int max_nodes = getNumNumaNodes();
            if (numa_available() == -1 || numa_node < 0 || numa_node >= max_nodes) {
                spdlog::warn("[NUMA] Invalid NUMA node: {}", numa_node);
                return -1;
            }

            struct bitmask* cpumask = numa_allocate_cpumask();
            if (!cpumask) return -1;

            numa_node_to_cpus(numa_node, cpumask);
            
            // Find first available CPU on this node
            int cpu_id = -1;
            unsigned int size = numa_bitmask_nbytes(cpumask) * 8;
            for (unsigned int i = 0; i < size; i++) {
                if (numa_bitmask_isbitset(cpumask, i)) {
                    cpu_id = i;
                    break;
                }
            }
            
            numa_free_cpumask(cpumask);

            if (cpu_id != -1 && bindThreadToCPU(cpu_id)) {
                spdlog::info("[NUMA] Thread bound to NUMA node {} (CPU {})", 
                    numa_node, cpu_id);
                return cpu_id;
            }
        #endif
        return -1;
    }

    /**
     * @brief Get CPUs available on NUMA node
     * @param numa_node NUMA node ID
     * @return Vector of CPU IDs on node
     */
    static std::vector<int> getCPUsOnNode(int numa_node) {
        std::vector<int> cpus;
        #ifdef __linux__
            int max_nodes = getNumNumaNodes();
            if (numa_available() != -1 && numa_node >= 0 && numa_node < max_nodes) {
                struct bitmask* cpumask = numa_allocate_cpumask();
                if (cpumask) {
                    numa_node_to_cpus(numa_node, cpumask);
                    unsigned int size = numa_bitmask_nbytes(cpumask) * 8;
                    for (unsigned int i = 0; i < size; i++) {
                        if (numa_bitmask_isbitset(cpumask, i)) {
                            cpus.push_back(i);
                        }
                    }
                    numa_free_cpumask(cpumask);
                }
            }
        #endif
        return cpus;
    }

    /**
     * @brief Allocate memory on specific NUMA node
     * @param size Number of bytes to allocate
     * @param numa_node NUMA node ID
     * @return Pointer to allocated memory, nullptr if failed
     */
    static void* allocateOnNode(size_t size, int numa_node) {
        #ifdef __linux__
            int max_nodes = getNumNumaNodes();
            if (numa_available() != -1 && numa_node >= 0 && numa_node < max_nodes) {
                void* ptr = numa_alloc_onnode(size, numa_node);
                if (ptr) {
                    spdlog::debug("[NUMA] Allocated {} bytes on NUMA node {}", size, numa_node);
                }
                return ptr;
            }
        #endif
        return nullptr;
    }

    /**
     * @brief Free NUMA-allocated memory
     * @param ptr Pointer to memory
     * @param size Size in bytes
     */
    static void freeNumaMemory(void* ptr, size_t size) {
        #ifdef __linux__
            if (ptr && numa_available() != -1) {
                numa_free(ptr, size);
            }
        #endif
    }

    /**
     * @brief Get NUMA node for current memory location
     * @param ptr Pointer to memory
     * @return NUMA node ID, -1 if failed
     */
    static int getMemoryNode(void* ptr) {
        #ifdef __linux__
            if (numa_available() != -1) {
                int node = -1;
                if (get_mempolicy(&node, nullptr, 0, ptr, MPOL_F_NODE | MPOL_F_ADDR) == 0) {
                    return node;
                }
            }
        #endif
        return -1;
    }

    /**
     * @brief Set memory policy to bind to NUMA node
     * @param ptr Pointer to memory region
     * @param size Size in bytes
     * @param numa_node NUMA node ID
     * @return true if successful
     */
    static bool setMemoryPolicy(void* ptr, size_t size, int numa_node) {
        #ifdef __linux__
            if (numa_available() == -1) return false;
            
            struct bitmask* nodeset = numa_bitmask_alloc(getNumNumaNodes());
            if (!nodeset) return false;

            numa_bitmask_clearall(nodeset);
            numa_bitmask_setbit(nodeset, numa_node);

            int ret = mbind(ptr, size, MPOL_BIND, nodeset->maskp, nodeset->size + 1, 0);
            numa_bitmask_free(nodeset);

            if (ret == 0) {
                spdlog::debug("[NUMA] Memory policy set for {} bytes on node {}", size, numa_node);
                return true;
            }
        #endif
        return false;
    }

    /**
     * @brief Print NUMA topology information
     */
    static void printTopology() {
        #ifdef __linux__
            if (numa_available() == -1) {
                spdlog::info("[NUMA] NUMA not available on this system");
                return;
            }

            int num_nodes = getNumNumaNodes();
            spdlog::info("[NUMA] ════════════════════════════════════");
            spdlog::info("[NUMA] NUMA Topology (nodes={})", num_nodes);
            spdlog::info("[NUMA] ════════════════════════════════════");

            for (int node = 0; node < num_nodes; node++) {
                struct bitmask* cpumask = numa_allocate_cpumask();
                if (cpumask) {
                    numa_node_to_cpus(node, cpumask);
                    
                    // Get memory size
                    long size = numa_node_size64(node, nullptr);
                    
                    spdlog::info("[NUMA] Node {}: {}MB, CPUs: {}",
                        node, size / (1024*1024),
                        bitmaskToString(cpumask));
                    
                    numa_free_cpumask(cpumask);
                }
            }
            spdlog::info("[NUMA] ════════════════════════════════════");
        #else
            spdlog::info("[NUMA] NUMA not available (not Linux)");
        #endif
    }

private:
    /**
     * @brief Convert CPU bitmask to string representation
     */
    static std::string bitmaskToString(struct bitmask* mask) {
        std::string result;
        bool first = true;
        unsigned int size = numa_bitmask_nbytes(mask) * 8;
        for (unsigned int i = 0; i < size; i++) {
            if (numa_bitmask_isbitset(mask, i)) {
                if (!first) result += ",";
                result += std::to_string(i);
                first = false;
            }
        }
        return result;
    }
};

} // namespace EventStream
