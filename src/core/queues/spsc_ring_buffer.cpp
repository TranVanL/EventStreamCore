#include <eventstream/core/queues/spsc_ring_buffer.hpp>

template<typename T , size_t Capacity>
bool SpscRingBuffer<T, Capacity>::push(const T& item){
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

template<typename T , size_t Capacity>
std::optional<T> SpscRingBuffer<T, Capacity>::pop(){
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

template<typename T , size_t Capacity>
std::size_t SpscRingBuffer<T, Capacity>::SizeUsed() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (Capacity + head - tail);
}

// Explicit instantiation for EventPtr (shared_ptr<Event>) - used by EventBusMulti
#include <eventstream/core/events/event.hpp>
template class SpscRingBuffer<std::shared_ptr<EventStream::Event>, 16384>;

// Explicit instantiation for benchmark - std::pair<uint64_t, uint64_t>
template class SpscRingBuffer<std::pair<uint64_t, uint64_t>, 16384>;

// Explicit instantiation for event pool benchmark - HighPerformanceEvent*
#include <eventstream/core/events/event_hp.hpp>
template class SpscRingBuffer<eventstream::core::HighPerformanceEvent*, 16384>;
