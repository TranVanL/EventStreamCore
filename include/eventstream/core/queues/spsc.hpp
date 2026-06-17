#pragma once
#include <atomic>
#include <cstddef>
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

    bool push(const T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next = (head + 1) & (Capacity - 1);
        if (next == tail_.load(std::memory_order_acquire)) {
            // Buffer is full
            return false;
        }
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            // Buffer is empty
            return std::nullopt;
        }
        T item = buffer_[tail];
        tail = (tail + 1) & (Capacity - 1);
        tail_.store(tail, std::memory_order_release);
        return item;
    }

    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (Capacity + head - tail);
    }

private:
    alignas(64) T buffer_[Capacity];
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};