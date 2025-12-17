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
    
    size_t SizeUsed() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (Capacity + head - tail);
    }

private: 
    T buffer_[Capacity];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

};