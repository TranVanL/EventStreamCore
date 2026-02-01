#pragma once 
#include <atomic>
#include <cstdint>
#include <optional> 

template<typename T , size_t Capacity>
class SpscRingBuffer {
    // FIX: Capacity MUST be power of 2 for bitwise AND masking to work correctly
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity > 0, "Capacity must be greater than 0");

public: 
    SpscRingBuffer() = default;
    ~SpscRingBuffer() = default;

    bool push(const T& item);
    std::optional<T> pop();
    size_t SizeUsed() const ;

private: 
    // Data buffer - aligned to cache line for optimal access
    alignas(64) T buffer_[Capacity];
    
    // Producer writes head_, Consumer reads head_
    // Cache line padding prevents false sharing with buffer_
    alignas(64) std::atomic<size_t> head_{0};
    
    // Consumer writes tail_, Producer reads tail_
    // Cache line padding prevents false sharing with head_
    alignas(64) std::atomic<size_t> tail_{0};

};