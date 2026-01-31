// ============================================================================
// LOCK-FREE DEDUPLICATOR TEST SUITE
// ============================================================================
// Tests for lock-free deduplication map
// - Basic insertion and lookup
// - Duplicate detection
// - Concurrent access safety
// - Cleanup and expiration
// ============================================================================

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <eventstream/core/queues/lock_free_dedup.hpp>

using namespace EventStream;

// ============================================================================
// TEST CLASS
// ============================================================================
class LockFreeDeduplicatorTest : public ::testing::Test {
protected:
    LockFreeDeduplicator dedup{1024};  // 1024 buckets for testing
    
    uint64_t get_current_ms() {
        return std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    }
};

// ============================================================================
// BASIC FUNCTIONALITY TESTS
// ============================================================================

TEST_F(LockFreeDeduplicatorTest, SingleInsertionAndLookup) {
    uint64_t now = get_current_ms();
    uint32_t event_id = 42;
    
    // First insert should succeed (return true = new)
    EXPECT_TRUE(dedup.insert(event_id, now));
    
    // Duplicate check should find it
    EXPECT_TRUE(dedup.is_duplicate(event_id, now));
}

TEST_F(LockFreeDeduplicatorTest, DuplicateInsertionRejected) {
    uint64_t now = get_current_ms();
    uint32_t event_id = 123;
    
    // First insertion
    EXPECT_TRUE(dedup.insert(event_id, now));
    
    // Second insertion with same ID should fail (return false = duplicate)
    EXPECT_FALSE(dedup.insert(event_id, now));
}

TEST_F(LockFreeDeduplicatorTest, NonExistentEventNotFound) {
    uint64_t now = get_current_ms();
    uint32_t event_id = 999;
    
    // Event was never inserted, so is_duplicate should return false
    EXPECT_FALSE(dedup.is_duplicate(event_id, now));
}

TEST_F(LockFreeDeduplicatorTest, MultipleUniqueEventsInserted) {
    uint64_t now = get_current_ms();
    
    // Insert multiple different event IDs
    for (uint32_t i = 0; i < 100; ++i) {
        EXPECT_TRUE(dedup.insert(i, now)) << "Failed to insert event " << i;
    }
    
    // All should be found as duplicates
    for (uint32_t i = 0; i < 100; ++i) {
        EXPECT_TRUE(dedup.is_duplicate(i, now)) << "Event " << i << " not found";
    }
    
    // Non-inserted events should not be found
    EXPECT_FALSE(dedup.is_duplicate(999, now));
    EXPECT_FALSE(dedup.is_duplicate(1000, now));
}

TEST_F(LockFreeDeduplicatorTest, ApproxSizeAccuracy) {
    uint64_t now = get_current_ms();
    
    // Start with empty
    EXPECT_EQ(dedup.approx_size(), 0);
    
    // Insert 50 events
    for (uint32_t i = 0; i < 50; ++i) {
        dedup.insert(i, now);
    }
    
    // Size should be approximately 50
    size_t size = dedup.approx_size();
    EXPECT_GE(size, 45);  // Allow some variance
    EXPECT_LE(size, 55);
}

// ============================================================================
// CLEANUP AND EXPIRATION TESTS
// ============================================================================

TEST_F(LockFreeDeduplicatorTest, ExpiredEntriesAreRemoved) {
    uint64_t old_time = 1000;  // Old timestamp
    uint64_t current_time = old_time + LockFreeDeduplicator::IDEMPOTENT_WINDOW_MS + 1000;
    
    uint32_t old_event = 100;
    uint32_t new_event = 200;
    
    // Insert event with old timestamp
    dedup.insert(old_event, old_time);
    
    // Insert event with current timestamp
    dedup.insert(new_event, current_time);
    
    // Before cleanup, both should exist
    EXPECT_TRUE(dedup.is_duplicate(old_event, current_time));
    EXPECT_TRUE(dedup.is_duplicate(new_event, current_time));
    
    // Run cleanup
    dedup.cleanup(current_time);
    
    // Old event should be removed, new should remain
    EXPECT_FALSE(dedup.is_duplicate(old_event, current_time));
    EXPECT_TRUE(dedup.is_duplicate(new_event, current_time));
}

TEST_F(LockFreeDeduplicatorTest, CleanupAllRemovesEverything) {
    uint64_t now = get_current_ms();
    
    // Insert 50 events
    for (uint32_t i = 0; i < 50; ++i) {
        dedup.insert(i, now);
    }
    
    EXPECT_GT(dedup.approx_size(), 0);
    
    // Cleanup all
    dedup.cleanup_all();
    
    // Map should be empty
    EXPECT_EQ(dedup.approx_size(), 0);
}

// ============================================================================
// CONCURRENT INSERTION TESTS
// ============================================================================

TEST_F(LockFreeDeduplicatorTest, ConcurrentInsertion) {
    uint64_t now = get_current_ms();
    std::atomic<int> success_count{0};
    std::atomic<int> duplicate_count{0};
    
    const int NUM_THREADS = 4;
    const int EVENTS_PER_THREAD = 25;
    
    auto inserter = [&](int thread_id) {
        for (int i = 0; i < EVENTS_PER_THREAD; ++i) {
            uint32_t event_id = thread_id * 1000 + i;
            if (dedup.insert(event_id, now)) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                duplicate_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    // Spawn multiple threads
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(inserter, i);
    }
    
    // Wait for all threads
    for (auto& t : threads) {
        t.join();
    }
    
    // Each event should be inserted exactly once across all threads
    EXPECT_EQ(success_count.load(), NUM_THREADS * EVENTS_PER_THREAD);
    EXPECT_EQ(duplicate_count.load(), 0);
}

TEST_F(LockFreeDeduplicatorTest, ConcurrentInsertionAndLookup) {
    uint64_t now = get_current_ms();
    std::atomic<int> lookup_success{0};
    
    const int NUM_INSERTERS = 2;
    const int NUM_LOOKERS = 2;
    const int EVENTS = 50;
    
    auto inserter = [&](int thread_id) {
        for (int i = 0; i < EVENTS; ++i) {
            uint32_t event_id = thread_id * 1000 + i;
            dedup.insert(event_id, now);
        }
    };
    
    auto looker = [&]() {
        // Try to find recently inserted events
        for (int i = 0; i < EVENTS; ++i) {
            for (int t = 0; t < NUM_INSERTERS; ++t) {
                uint32_t event_id = t * 1000 + i;
                if (dedup.is_duplicate(event_id, now)) {
                    lookup_success.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };
    
    // Spawn inserters and lookers
    std::vector<std::thread> threads;
    
    for (int i = 0; i < NUM_INSERTERS; ++i) {
        threads.emplace_back(inserter, i);
    }
    
    // Give inserters a bit of head start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    for (int i = 0; i < NUM_LOOKERS; ++i) {
        threads.emplace_back(looker);
    }
    
    // Wait for all
    for (auto& t : threads) {
        t.join();
    }
    
    // Should have found at least some of the inserted events
    EXPECT_GT(lookup_success.load(), 0);
}

TEST_F(LockFreeDeduplicatorTest, HighContentionInsertion) {
    uint64_t now = get_current_ms();
    std::atomic<int> success_count{0};
    std::atomic<int> duplicate_count{0};
    
    const int NUM_THREADS = 8;
    const int ITERATIONS = 50;
    
    // All threads try to insert the SAME event ID
    uint32_t contested_event = 12345;
    
    auto inserter = [&]() {
        for (int i = 0; i < ITERATIONS; ++i) {
            if (dedup.insert(contested_event, now)) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                duplicate_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(inserter);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // Exactly one thread should succeed (and maybe retry, but only first insert succeeds)
    // Others should all get duplicates
    int total_attempts = NUM_THREADS * ITERATIONS;
    EXPECT_EQ(success_count.load(), 1);
    EXPECT_LE(duplicate_count.load(), total_attempts - 1);
}

// ============================================================================
// STRESS TEST
// ============================================================================

TEST_F(LockFreeDeduplicatorTest, StressTestManyEvents) {
    uint64_t now = get_current_ms();
    const int NUM_EVENTS = 5000;
    
    // Insert many events
    for (uint32_t i = 0; i < NUM_EVENTS; ++i) {
        bool inserted = dedup.insert(i, now);
        EXPECT_TRUE(inserted);
    }
    
    // Verify all were inserted
    for (uint32_t i = 0; i < NUM_EVENTS; ++i) {
        EXPECT_TRUE(dedup.is_duplicate(i, now));
    }
    
    // Cleanup should handle many entries
    dedup.cleanup(now + LockFreeDeduplicator::IDEMPOTENT_WINDOW_MS + 1000);
    
    // All should be removed after expiration
    EXPECT_EQ(dedup.approx_size(), 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
