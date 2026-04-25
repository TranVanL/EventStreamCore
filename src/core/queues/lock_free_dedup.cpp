#include <eventstream/core/queues/lock_free_dedup.hpp>
#include <spdlog/spdlog.h>

namespace EventStream {

bool LockFreeDeduplicator::insert(uint32_t event_id, uint64_t now_ms) {
    size_t bucket_idx = event_id % buckets_.size();
    
    // Pre-allocate entry outside the retry loop to avoid repeated allocations
    Entry* new_entry = new Entry(event_id, now_ms);
    
    // Retry loop for CAS contention (rare on low contention)
    int retry_count = 0;
    const int MAX_RETRIES = 3;  // Reasonable limit before giving up
    
    while (retry_count < MAX_RETRIES) {
        // Read head pointer with acquire semantics
        Entry* head = buckets_[bucket_idx].load(std::memory_order_acquire);
        
        // Check for duplicate in chain
        Entry* curr = head;
        while (curr) {
            if (curr->id == event_id) {
                // Duplicate found - cleanup and return false
                delete new_entry;
                return false;
            }
            curr = curr->next;
        }
        
        // Prepare insertion: link new entry to current head
        new_entry->next = head;
        
        // Try to CAS new entry as new head (lock-free)
        // Memory ordering: acq_rel on success ensures:
        // - All writes to new_entry are visible before CAS (release)
        // - All subsequent reads see the updated head (acquire)
        if (buckets_[bucket_idx].compare_exchange_strong(
                head, new_entry,
                std::memory_order_acq_rel,      // Success: acquire-release for full barrier
                std::memory_order_acquire)) {   // Failure: re-read head
            // Success! Entry inserted at head of bucket
            spdlog::debug("[LockFreeDedup] Inserted event_id={} to bucket {}", 
                         event_id, bucket_idx);
            return true;
        }
        
        // CAS failed - another thread inserted first, retry
        retry_count++;
    }
    
    // Max retries exceeded - this is very rare, indicates extreme contention
    spdlog::warn("[LockFreeDedup] Max retries exceeded for event_id={}, giving up", 
                event_id);
    delete new_entry;
    return false;
}

// is_duplicate() is inline in the header

void LockFreeDeduplicator::cleanup(uint64_t now_ms) {
    size_t total_removed = 0;
    uint64_t start_time = std::chrono::system_clock::now().time_since_epoch().count();
    
    for (size_t i = 0; i < buckets_.size(); ++i) {
        total_removed += cleanup_bucket_count(i, now_ms);
    }
    
    uint64_t end_time = std::chrono::system_clock::now().time_since_epoch().count();
    uint64_t elapsed_us = (end_time - start_time) / 1000;  // Convert to microseconds
    
    if (total_removed > 0 || elapsed_us > 1000) {  // Log if removed entries or took > 1ms
        spdlog::info("[LockFreeDedup] Cleanup: removed={} entries, took={}us, size={}", 
                    total_removed, elapsed_us, approx_size());
    }
}

// Remove expired entries from a single bucket.
// Uses CAS for head updates to stay safe with concurrent inserts.
size_t LockFreeDeduplicator::cleanup_bucket_count(size_t bucket_idx, uint64_t now_ms) {
    Entry* head = buckets_[bucket_idx].load(std::memory_order_acquire);
    
    // Fast path: empty bucket
    if (head == nullptr) {
        return 0;
    }
    
    // Phase 1: remove expired entries from the head of the chain (needs CAS)
    size_t head_retries = 0;
    constexpr size_t MAX_HEAD_RETRIES = 10;  // Prevent infinite loop on high contention
    
    while (head != nullptr && head_retries < MAX_HEAD_RETRIES) {
        uint64_t age_ms = (now_ms >= head->timestamp_ms) 
                         ? (now_ms - head->timestamp_ms)
                         : 0;
        
        if (age_ms <= IDEMPOTENT_WINDOW_MS) {
            break;  // Head is not expired, stop
        }
        
        Entry* next = head->next;
        
        // Try to CAS head to next (thread-safe with insert())
        // Use acq_rel for proper memory ordering
        if (buckets_[bucket_idx].compare_exchange_strong(
                head, next,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // Success - delete old head
            delete head;
            head = next;
            head_retries = 0;  // Reset retry counter on success
        } else {
            // CAS failed - head was updated by insert(), re-check if still expired
            head_retries++;
        }
    }
    
    // Phase 2: remove expired entries from the middle/tail
    // Only one cleanup thread runs at a time, so touching next ptrs is safe.
    if (head == nullptr) {
        return 0;
    }
    
    Entry* prev = head;
    Entry* curr = head->next;
    size_t removed = 0;
    
    while (curr != nullptr) {
        uint64_t age_ms = (now_ms >= curr->timestamp_ms) 
                         ? (now_ms - curr->timestamp_ms)
                         : 0;
        
        if (age_ms > IDEMPOTENT_WINDOW_MS) {
            // Expired - unlink and delete
            Entry* next = curr->next;
            prev->next = next;  // Safe: insert() doesn't touch prev->next
            
            // Memory fence before delete: ensure concurrent readers
            // have finished reading curr. Not fully safe without hazard
            // pointers or RCU, but good enough for single-cleanup-thread.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            
            delete curr;
            curr = next;
            removed++;
        } else {
            // Keep this entry
            prev = curr;
            curr = curr->next;
        }
    }
    
    return removed;
}

// approx_size() is inline in the header

void LockFreeDeduplicator::cleanup_all() {
    size_t total_removed = 0;
    
    for (size_t i = 0; i < buckets_.size(); ++i) {
        total_removed += cleanup_bucket_count(i, UINT64_MAX);  // UINT64_MAX = always expired
    }
    
    spdlog::info("[LockFreeDedup] Cleanup all: removed={} entries", total_removed);
}

}  // namespace EventStream
