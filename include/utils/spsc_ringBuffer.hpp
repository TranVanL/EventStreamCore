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

private: 
    T buffer_[Capacity];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

};