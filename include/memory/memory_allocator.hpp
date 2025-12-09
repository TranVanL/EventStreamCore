#pragma once
#include "event_memorypool.hpp"

template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    MemoryPool* pool;

    PoolAllocator(MemoryPool* p = nullptr) noexcept : pool(p) {}
    template <class U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept : pool(other.pool) {}

    T* allocate(std::size_t n) {
        if (!pool || n != 1) {
            return static_cast<T*>(::operator new(n * sizeof(T)));
        }

        if (sizeof(T) > pool->getBlockSize()) {
            return static_cast<T*>(::operator new(n * sizeof(T)));
        }

        void* block = pool->allocate();
        if (!block) {
            return static_cast<T*>(::operator new(n * sizeof(T)));
        }
        return static_cast<T*>(block);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (!pool || n != 1 || sizeof(T) > pool->getBlockSize()) {
            ::operator delete(p);
            return;
        }
        pool->deallocate(p);
    }
};

template <class T, class U>
bool operator==(const PoolAllocator<T>& a, const PoolAllocator<U>& b) {
    return a.pool == b.pool;
}

template <class T, class U>
bool operator!=(const PoolAllocator<T>& a, const PoolAllocator<U>& b) {
    return a.pool != b.pool;
}
