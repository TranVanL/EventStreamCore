#pragma once
#include <atomic>
#include <cstdint>
#include <optional>

/**
 * @class SpscRingBuffer
 * @brief Lock-free single-producer single-consumer ring buffer.
 *
 * Capacity must be a power of 2 for efficient index masking.
 * Cache-line aligned head/tail to prevent false sharing.
 */
template<typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity > 0, "Capacity must be greater than 0");

public:
    SpscRingBuffer() = default;
    ~SpscRingBuffer() = default;

    bool push(const T& item);
    std::optional<T> pop();
    size_t size() const;

private:
    alignas(64) T buffer_[Capacity];
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};