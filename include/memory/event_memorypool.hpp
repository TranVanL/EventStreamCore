#pragma once
#include <vector>
#include <mutex>
#include <cstddef>
#include <memory>
#include <cassert>
#include <new>
#include <type_traits>

class MemoryPool {
public:
    explicit MemoryPool(size_t blockSize, size_t blockCount)
        : blockSize(blockSize), blockCount(blockCount) 
    {
    
        if (blockSize < alignof(std::max_align_t)) {
            blockSize = alignof(std::max_align_t);
        }

        pool.resize(blockSize * blockCount);
        freeList.reserve(blockCount);

        for (size_t i = 0; i < blockCount; i++) {
            freeList.push_back(pool.data() + i * blockSize);
        }
    }

    size_t getBlockSize() const { return blockSize; }

    void* allocate() {
        std::lock_guard<std::mutex> lock(mu);
        if (freeList.empty()) return nullptr;
        void* p = freeList.back();
        freeList.pop_back();
        return p;
    }

    void deallocate(void* ptr) {
        std::lock_guard<std::mutex> lock(mu);
        uint8_t* start = pool.data();
        uint8_t* end   = pool.data() + pool.size();
        auto p = reinterpret_cast<uint8_t*>(ptr);
        assert(p >= start && p < end);
        assert(((p - start) % blockSize) == 0); 

        freeList.push_back(p);
    }

private:
    size_t blockSize;
    size_t blockCount;
    std::vector<uint8_t> pool;
    std::vector<uint8_t*> freeList;
    std::mutex mu;
};
