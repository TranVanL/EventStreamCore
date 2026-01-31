// ============================================================================
// LOCK-FREE DEDUPLICATION MAP
// ============================================================================
// Day 33: Optimize TransactionalProcessor idempotency check
//
// Replaces: std::mutex + std::unordered_map lookup
// With: Atomic-based concurrent hash map with minimal locking
//
// Design:
// - Buckets as atomic<Entry*> for lock-free reads
// - CAS-based insertion (no lock required for read path)
// - Separate cleanup thread for periodical eviction
// - Memory pooling to avoid malloc in hot path
// ============================================================================

#pragma once

#include <cstdint>
#include <atomic>
#include <vector>
#include <memory>
#include <chrono>
#include <spdlog/spdlog.h>

namespace EventStream {

class LockFreeDeduplicator {
public:
    struct Entry {
        uint32_t id;
        uint64_t timestamp_ms;
        Entry* next;  // For chaining in same bucket
        
        Entry(uint32_t id_, uint64_t ts_)
            : id(id_), timestamp_ms(ts_), next(nullptr) {}
    };
    
    static constexpr size_t DEFAULT_BUCKETS = 4096;
    static constexpr uint64_t IDEMPOTENT_WINDOW_MS = 3600000;  // 1 hour
    
    explicit LockFreeDeduplicator(size_t num_buckets = DEFAULT_BUCKETS)
        : buckets_(num_buckets) {
        for (auto& bucket : buckets_) {
            bucket.store(nullptr, std::memory_order_release);
        }
    }
    
    ~LockFreeDeduplicator() {
        cleanup_all();
    }
    
    // Check if event already processed (lock-free read path)
    // Returns: true if duplicate (already seen), false if new
    bool is_duplicate(uint32_t event_id, uint64_t now_ms) {
        size_t bucket_idx = event_id % buckets_.size();
        Entry* entry = buckets_[bucket_idx].load(std::memory_order_acquire);
        
        while (entry) {
            if (entry->id == event_id) {
                return true;  // Duplicate!
            }
            entry = entry->next;
        }
        return false;  // New event
    }
    
    // Insert event into dedup map (lock-free CAS path)
    // Returns: true if inserted (new), false if already existed
    bool insert(uint32_t event_id, uint64_t now_ms);
    
    // Periodic cleanup - remove entries older than IDEMPOTENT_WINDOW_MS
    // This is called by a separate cleanup thread (not in hot path)
    void cleanup(uint64_t now_ms);
    
    // Force cleanup all entries (used on shutdown)
    void cleanup_all();
    
    // Get current map size (approximate)
    size_t approx_size() const {
        size_t count = 0;
        for (size_t i = 0; i < buckets_.size(); ++i) {
            Entry* entry = buckets_[i].load(std::memory_order_acquire);
            while (entry) {
                ++count;
                entry = entry->next;
            }
        }
        return count;
    }
    
private:
    std::vector<std::atomic<Entry*>> buckets_;
    
    // Helper method to clean expired entries from a single bucket
    size_t cleanup_bucket_count(size_t bucket_idx, uint64_t now_ms);
};

}  // namespace EventStream
