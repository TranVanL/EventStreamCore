# Code Optimization Analysis & Improvements

## Performance Issues Identified

### 1. **MetricRegistry::updateEventTimestamp() - Called 3x per event** ⚠️
**Current Implementation**:
```cpp
void MetricRegistry::updateEventTimestamp(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx_);  // Lock acquisition on hot path
    auto it = metrics_map_.find(name);
    if (it != metrics_map_.end()) {
        it->second.last_event_timestamp_ms.store(now(), std::memory_order_relaxed);
    }
}
```

**Problem**:
- Called 3x per event: push (EventBusMulti) + pop (EventBusMulti) + process (processor)
- Acquires mutex + hash map lookup + function call
- For 82K events/sec = 246K lock operations/sec
- Estimates: ~10-15% CPU overhead

**Optimization**: Use thread-local timestamp buffer + periodic batched update

---

### 2. **EventBusMulti::dropBatchFromQueue() - Excessive allocations**
**Problem**:
```cpp
size_t EventBusMulti::dropBatchFromQueue(QueueId q) {
    // ...
    std::vector<EventPtr> batch;  // Line 169 - vector created every call
    // ...
}
```

**Overhead**: Vector constructor/destructor on every call (even if not used often)

**Fix**: Use preallocated static buffer or remove if unused

---

### 3. **Metrics Calculation - O(n) map iteration**
**Problem**:
```cpp
std::unordered_map<std::string, MetricSnapshot> MetricRegistry::getSnapshots() {
    std::unordered_map<std::string, MetricSnapshot> snaps;  // Create new map
    for (auto& [name, metrics] : metrics_map_) {            // Iterate all entries
        // ...
    }
    return snaps;  // Copy on return
}
```

**Impact**: Called every metric reporting cycle, creates temporary maps

**Fix**: Use reference return or move semantics

---

### 4. **Lock-Free Dedup - Unnecessary delete on failure path**
**Problem**:
```cpp
// src/utils/lock_free_dedup.cpp line 39, 54, 81
Entry* new_entry = new Entry(event_id, now_ms);  // Line 39
// ...
if (MAX_RETRIES exceeded) {
    delete new_entry;  // Line 81 - Delete on failure
    return false;
}
```

**Impact**: Single allocation/deallocation on rare retry exhaustion. Low impact but unnecessary.

**Fix**: Use placement new on preallocated pool or object_pool

---

### 5. **String copies in hot paths**
**Problem**:
```cpp
// EventBusMulti metrics lookup
const std::string name_str = "EventBusMulti";  // String copy/creation
auto& m = MetricRegistry::getInstance().getMetrics(name_str);  // Pass by ref

// Processors
auto &m = MetricRegistry::getInstance().getMetrics(name());  // Virtual call + string
```

**Overhead**: String construction/destruction in hot paths

**Fix**: Use string_view or compile-time hashing

---

### 6. **TCP Parser - Vector copying**
**Problem**:
```cpp
// src/ingest/tcp_parser.cpp line 65
std::vector<uint8_t> frame_body(
    full_frame_include_length.begin() + 4,  // Vector copy constructor
    full_frame_include_length.end());
```

**Overhead**: Deep copy of payload data on every parse

**Fix**: Use string_view or reference-based parsing

---

### 7. **Admin Loop - Unordered map iteration**
**Problem**:
```cpp
// src/admin/admin_loop.cpp line 58
void Admin::reportMetrics(const std::unordered_map<std::string, MetricSnapshot>& snapshots) {
    // Multiple iterations over snapshots map
}
```

**Overhead**: Multiple passes over potentially large hash map

**Fix**: Single-pass iteration with direct output

---

### 8. **Batch Processor - Double map lookup**
**Problem**:
```cpp
// src/event_processor/batch_processor.cpp
std::unique_lock<std::mutex> map_lock(buckets_mutex_);
auto [it, inserted] = buckets_.try_emplace(event.topic);  // First lookup
TopicBucket& bucket = it->second;
map_lock.unlock();
// ...
auto& last = last_flush_[event.topic];  // Second lookup in different map
```

**Overhead**: Two separate maps with same key, cache misses

**Fix**: Combine maps or use single lookup

---

## Optimization Priority

| Priority | Issue | Estimated Gain | Effort |
|---|---|---|---|
| 🔴 HIGH | MetricRegistry::updateEventTimestamp | 10-15% CPU | Medium |
| 🟠 MED | TCP parser vector copying | 5-8% | Low |
| 🟠 MED | Metrics map iteration | 3-5% | Low |
| 🟡 LOW | Lock-free dedup delete | <1% | Very Low |
| 🟡 LOW | String copies | 2-3% | Medium |
| 🟡 LOW | Batch processor double lookup | 1-2% | Low |

---

## Detailed Optimization Solutions

### Solution 1: Async Metric Timestamp Updates
**File**: `src/metrics/metricRegistry.cpp`

**Current** (3 mutex acquires per event):
```cpp
MetricRegistry::getInstance().updateEventTimestamp("EventBusMulti");  // Push
MetricRegistry::getInstance().updateEventTimestamp("EventBusMulti");  // Pop  
MetricRegistry::getInstance().updateEventTimestamp(name());            // Process
```

**Optimized** (zero synchronous updates):
```cpp
// Use thread-local write-ahead buffer
thread_local uint64_t last_update_ns = 0;
if (EventStream::nowNs() - last_update_ns > 1'000'000) {  // Update every 1ms
    MetricRegistry::getInstance().updateEventTimestamp(name());
    last_update_ns = EventStream::nowNs();
}
```

**Expected Gain**: 10-15% CPU reduction  
**Risk**: Minimal (timestamp only updated every 1ms instead of every event)

---

### Solution 2: Remove/Inline Unused dropBatchFromQueue vector
**File**: `src/event/EventBusMulti.cpp`

**Check usage**:
```bash
grep -r "dropBatchFromQueue" src/ include/
```

If only called rarely or from BatchProcessor, optimize:
```cpp
// Remove local vector allocation
for (size_t i = 0; i < DROP_BATCH_SIZE; ++i) {
    if (auto evt = ringBuffer.pop()) {
        dropped++;
    } else {
        break;
    }
}
```

**Expected Gain**: <1% (low impact but clean)

---

### Solution 3: Move Semantics for Metrics Maps
**File**: `src/metrics/metricRegistry.cpp`

**Before**:
```cpp
std::unordered_map<std::string, MetricSnapshot> MetricRegistry::getSnapshots() {
    std::unordered_map<std::string, MetricSnapshot> snaps;
    for (auto& [name, metrics] : metrics_map_) {
        snaps.emplace(name, buildSnapshot(...));
    }
    return snaps;  // Copy
}
```

**After**:
```cpp
void MetricRegistry::getSnapshots(std::unordered_map<std::string, MetricSnapshot>& snaps) {
    snaps.clear();
    for (auto& [name, metrics] : metrics_map_) {
        snaps.emplace(name, buildSnapshot(...));
    }
    // Output parameter avoids copy
}
```

**Expected Gain**: 2-3% when metrics reported (out-of-hot-path)

---

### Solution 4: Replace TCP Vector Copy with string_view Parsing
**File**: `src/ingest/tcp_parser.cpp`

**Before**:
```cpp
std::vector<uint8_t> frame_body(
    full_frame.begin() + 4,
    full_frame.end());  // Deep copy
ParsedResult result = parseFrame(frame_body);
```

**After** (if parser can accept const_view):
```cpp
// Just pass references - no copy
span<const uint8_t> frame_body(
    full_frame.data() + 4,
    full_frame.size() - 4);
ParsedResult result = parseFrame(frame_body);
```

**Expected Gain**: 5-8% for TCP parsing path  
**Prerequisite**: Verify parseFrame doesn't need owned data

---

### Solution 5: Cache Metric Names as Compile-Time Strings
**File**: `include/metrics/metricRegistry.hpp`

**Before**:
```cpp
auto &m = MetricRegistry::getInstance().getMetrics(name());  // Virtual + string creation
```

**After** (for fixed names):
```cpp
// Compile-time processor names
static constexpr std::string_view REALTIME_NAME = "RealtimeProcessor";
static constexpr std::string_view TRANSACTIONAL_NAME = "TransactionalProcessor";

auto &m = MetricRegistry::getInstance().getMetrics(REALTIME_NAME);  // No allocation
```

**Expected Gain**: 1-2% for processor metrics  
**Complexity**: Low (mostly search-replace)

---

## Implementation Plan

1. ✅ **Phase 1 (High Impact)**: Implement Solution 1 (async timestamps) - 10-15% gain
2. ✅ **Phase 2 (Medium Impact)**: Solutions 2-3 (cleanup unused code, optimize returns) - 3-5% gain
3. ✅ **Phase 3 (Low Impact)**: Solutions 4-5 (string/vector optimization) - 5-8% gain

**Total Expected**: 18-28% CPU reduction for same throughput

---

## Testing Strategy

After each optimization:

```bash
# Build
cmake --build build -j$(nproc)

# Test
./build/unittest/EventStreamTests

# Benchmark (optional)
# ./build/benchmark/benchmark --comparison
```

**Success Criteria**:
- ✅ All 38 tests passing
- ✅ Zero warnings
- ✅ Same functionality
- ✅ Reduced lock overhead (measured via perf)

---

## Files to Modify

1. `src/metrics/metricRegistry.hpp` - Add batched timestamp buffering
2. `src/metrics/metricRegistry.cpp` - Implement solution 1
3. `src/event/EventBusMulti.cpp` - Remove unused vector allocation
4. `include/metrics/metricRegistry.hpp` - Add compile-time name constants
5. `src/ingest/tcp_parser.cpp` - Evaluate vector copying

---

## Rollback Plan

All changes are non-breaking and can be rolled back via git:
```bash
git diff HEAD~1  # See what changed
git revert <commit>  # Rollback if needed
```

---

**Status**: Analysis Complete  
**Next**: Implement optimizations Phase 1
