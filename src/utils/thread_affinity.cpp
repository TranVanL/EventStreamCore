#include "utils/thread_affinity.hpp"
#include <stdexcept>
#include <sched.h>

#ifdef __linux__
#include <pthread.h>
#elif _WIN32
#include <windows.h>
#endif

void pinThreadToCore(std::thread& t, int core_id) {
    // Implementation for pinning thread to a specific core
    if (!t.joinable()) {
        throw std::runtime_error("Thread is not joinable - cannot set affinity");
    }
    
    #ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        throw std::runtime_error("Error calling pthread_setaffinity_np: " + std::to_string(rc));
    }

    #elif _WIN32
    if (core_id < 0 || core_id >= 64) {
        throw std::runtime_error("Invalid core_id: " + std::to_string(core_id));
    }
    // On Windows, thread affinity is best effort - many systems don't support it
    // Silent fail is acceptable here
    DWORD_PTR mask = 1ULL << core_id;
    HANDLE threadHandle = (HANDLE)t.native_handle();
    if (threadHandle != NULL && threadHandle != INVALID_HANDLE_VALUE) {
        SetThreadAffinityMask(threadHandle, mask);
    }
    
    #else
    throw std::runtime_error("Thread affinity not supported on this platform");
    #endif
}