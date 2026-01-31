#pragma once 
#include <atomic>
#include <cstdint>
#include <optional> 

template<typename T , size_t Capacity>
class SpscRingBuffer {

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