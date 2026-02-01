#pragma once

#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace EventStream {

/**
 * @brief High-performance latency histogram using log2 buckets (Day 37)
 * 
 * Measures tail latency (p50, p99, p99.9) without locks:
 * - Bucket i covers: [2^i, 2^(i+1)) nanoseconds
 * - Fast O(1) bucket lookup via log2
 * - Thread-safe increment operations
 * - Offline percentile calculation via nth_element
 * 
 * Usage:
 *   auto hist = LatencyHistogram::create();
 *   uint64_t start_ns = now_ns();
 *   ... do work ...
 *   hist.record(now_ns() - start_ns);
 *   hist.printPercentiles();
 */
class LatencyHistogram {
public:
    // 64 buckets = covers 0-63 bits = up to 2^63 ns (~292 years)
    static constexpr size_t NUM_BUCKETS = 64;
    
    /**
     * @brief Create a new histogram instance
     */
    static LatencyHistogram create() {
        return LatencyHistogram();
    }

    /**
     * @brief Record a latency value (in nanoseconds)
     */
    void record(uint64_t latency_ns) {
        size_t bucket = bucketForLatency(latency_ns);
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        total_count_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Get total number of recorded samples
     */
    uint64_t getTotalCount() const {
        return total_count_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get count in a specific bucket
     */
    uint64_t getBucketCount(size_t bucket) const {
        if (bucket >= NUM_BUCKETS) return 0;
        return buckets_[bucket].load(std::memory_order_relaxed);
    }

    /**
     * @brief Calculate percentile (0-100)
     * Requires collecting samples into vector first
     */
    uint64_t calculatePercentile(double percentile) const {
        uint64_t total = getTotalCount();
        if (total == 0) return 0;

        // Collect all samples
        std::vector<uint64_t> samples;
        samples.reserve(total);
        
        for (size_t b = 0; b < NUM_BUCKETS; ++b) {
            uint64_t count = getBucketCount(b);
            uint64_t bucket_min = bucketMin(b);
            for (uint64_t i = 0; i < count; ++i) {
                // Use midpoint of bucket as representative value
                samples.push_back(bucket_min + (1ULL << b) / 2);
            }
        }

        // FIX: Check for empty samples to prevent buffer underflow
        if (samples.empty()) return 0;

        // Partial sort to find nth element
        size_t idx = static_cast<size_t>((percentile / 100.0) * samples.size());
        if (idx >= samples.size()) idx = samples.size() - 1;
        
        std::nth_element(samples.begin(), samples.begin() + idx, samples.end());
        return samples[idx];
    }

    /**
     * @brief Get min bucket value (lower bound)
     */
    uint64_t getMinValue() const {
        for (size_t b = 0; b < NUM_BUCKETS; ++b) {
            if (getBucketCount(b) > 0) {
                return bucketMin(b);
            }
        }
        return 0;
    }

    /**
     * @brief Get max bucket value (upper bound)
     * FIX: Use size_t with proper reverse iteration idiom to avoid signed/unsigned issues
     */
    uint64_t getMaxValue() const {
        for (size_t b = NUM_BUCKETS; b-- > 0; ) {
            if (getBucketCount(b) > 0) {
                return bucketMax(b);
            }
        }
        return 0;
    }

    /**
     * @brief Print percentile summary
     */
    void printPercentiles() const {
        uint64_t total = getTotalCount();
        if (total == 0) {
            spdlog::info("Latency Histogram: No samples recorded");
            return;
        }

        uint64_t p50 = calculatePercentile(50);
        uint64_t p99 = calculatePercentile(99);
        uint64_t p999 = calculatePercentile(99.9);
        uint64_t min_val = getMinValue();
        uint64_t max_val = getMaxValue();

        spdlog::info("╔════════════════════════════════════════╗");
        spdlog::info("║  LATENCY HISTOGRAM (p-percentiles)     ║");
        spdlog::info("╠════════════════════════════════════════╣");
        spdlog::info("║  Total Samples:     {:8}         ║", total);
        spdlog::info("║  Min Latency:       {:6} ns        ║", min_val);
        spdlog::info("║  p50 (Median):      {:6} ns        ║", p50);
        spdlog::info("║  p99:               {:6} ns        ║", p99);
        spdlog::info("║  p99.9:             {:6} ns        ║", p999);
        spdlog::info("║  Max Latency:       {:6} ns        ║", max_val);
        spdlog::info("╚════════════════════════════════════════╝");
    }

    /**
     * @brief Clear all buckets (for test reset)
     */
    void reset() {
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            buckets_[i].store(0, std::memory_order_relaxed);
        }
        total_count_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<uint64_t>, NUM_BUCKETS> buckets_{};
    std::atomic<uint64_t> total_count_{0};

    /**
     * @brief Get bucket index for a latency value
     * Uses log2(latency) to determine bucket
     * Bucket 0 = 0-1 ns, Bucket 1 = 2-3 ns, Bucket 2 = 4-7 ns, etc.
     * 
     * Note: Bucket boundaries are powers of 2:
     * Bucket 0: [0, 1]
     * Bucket 1: [2, 3]  
     * Bucket 2: [4, 7]
     * Bucket 3: [8, 15]
     * ...
     * Bucket k: [2^k, 2^(k+1) - 1]
     */
    static size_t bucketForLatency(uint64_t latency_ns) {
        if (latency_ns <= 1) return 0;  // Fix: 0 and 1 both go to bucket 0
        
        // Find position of most significant bit
        // For value X, this gives us the bucket for [2^k, 2^(k+1))
        // __builtin_clzll returns number of leading zeros in 64-bit value
        int msb = 63 - __builtin_clzll(latency_ns);
        
        // Clamp to valid range
        size_t bucket = std::min(static_cast<size_t>(msb), NUM_BUCKETS - 1);
        return bucket;
    }

    /**
     * @brief Get minimum value in bucket
     */
    static uint64_t bucketMin(size_t bucket) {
        if (bucket == 0) return 0;
        return 1ULL << bucket;
    }

    /**
     * @brief Get maximum value in bucket
     */
    static uint64_t bucketMax(size_t bucket) {
        if (bucket == 63) return UINT64_MAX;
        return (1ULL << (bucket + 1)) - 1;
    }
};

} // namespace EventStream
