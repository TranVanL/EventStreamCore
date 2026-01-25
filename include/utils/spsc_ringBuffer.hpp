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
    T buffer_[Capacity];
    std::atomic<size_t> head_{0};
    // Day 39: Cache line padding to prevent false sharing between head_ and tail_
    // x86-64 cache lines are 64 bytes, so we pad tail_ to different line
    alignas(64) std::atomic<size_t> tail_{0};

};