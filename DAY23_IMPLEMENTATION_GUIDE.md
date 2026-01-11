# 🛠️ Day 23 Implementation Guide - Ready-to-Code

**Status**: All code snippets prepared. Ready to implement.

---

## 📋 File Changes Required

| File | Action | Type |
|------|--------|------|
| `include/admin/ControlDecision.hpp` | CREATE | Header |
| `include/eventprocessor/event_processor.hpp` | MODIFY | Add ProcessorState |
| `src/admin/admin_loop.hpp` | MODIFY | Add method declarations |
| `src/admin/admin_loop.cpp` | MODIFY | Implement evaluateSnapshot() + executeDecision() |
| `src/event_processor/batch_processor.cpp` | MODIFY | Add state machine |
| `src/event_processor/realtime_processor.cpp` | MODIFY | Add state machine |
| `src/event_processor/transactional_processor.cpp` | MODIFY | Add state machine |
| `include/storage_engine/storage_engine.hpp` | MODIFY | Add appendDLQ() method |
| `src/storage_engine/storage_engine.cpp` | MODIFY | Implement appendDLQ() |

---

## 1️⃣ ControlDecision.hpp (NEW FILE)

**Location**: `include/admin/ControlDecision.hpp`

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace EventStream {

/**
 * @enum ControlAction
 * @brief Actions the admin can take when system health degrades
 */
enum class ControlAction : uint8_t {
    NONE = 0,                  // No action needed - system healthy
    PAUSE_PROCESSOR = 1,       // Stop processor from consuming events
    DROP_BATCH = 2,            // Drop N events to DLQ to reduce load
    DRAIN = 3,                 // Finish current work, then pause
    PUSH_DLQ = 4,              // Push failed events to DLQ
    RESUME = 5                 // Resume normal operation
};

/**
 * @enum FailureState
 * @brief System health classification
 */
enum class FailureState : uint8_t {
    HEALTHY = 0,      // All metrics normal
    DEGRADED = 1,     // Some metrics elevated
    CRITICAL = 2      // Metrics exceed critical thresholds
};

/**
 * @struct ControlDecision
 * @brief Formal decision object for control plane
 * 
 * Represents a decision made by AdminLoop based on metrics snapshot.
 * Separate from execution - can be logged, replayed, tested independently.
 */
struct ControlDecision {
    ControlAction action;           // What to do
    FailureState reason;            // Why it matters
    std::string details;            // Human-readable details (e.g., "Queue depth 12000 > limit 10000")
    uint64_t timestamp_ms{0};       // When decision was made
    
    ControlDecision() 
        : action(ControlAction::NONE), 
          reason(FailureState::HEALTHY),
          details("") {}
    
    ControlDecision(ControlAction a, FailureState r, const std::string& d)
        : action(a), reason(r), details(d) {}
    
    static const char* actionString(ControlAction a) {
        switch (a) {
            case ControlAction::NONE: return "NONE";
            case ControlAction::PAUSE_PROCESSOR: return "PAUSE_PROCESSOR";
            case ControlAction::DROP_BATCH: return "DROP_BATCH";
            case ControlAction::DRAIN: return "DRAIN";
            case ControlAction::PUSH_DLQ: return "PUSH_DLQ";
            case ControlAction::RESUME: return "RESUME";
            default: return "UNKNOWN";
        }
    }
    
    static const char* failureStateString(FailureState s) {
        switch (s) {
            case FailureState::HEALTHY: return "HEALTHY";
            case FailureState::DEGRADED: return "DEGRADED";
            case FailureState::CRITICAL: return "CRITICAL";
            default: return "UNKNOWN";
        }
    }
};

} // namespace EventStream
```

---

## 2️⃣ ProcessorState Addition

**Location**: `include/eventprocessor/event_processor.hpp` (ADD AT TOP)

```cpp
#pragma once
#include "event/EventBusMulti.hpp"
#include "metrics/metricRegistry.hpp"
#include <storage_engine/storage_engine.hpp>
#include "control/Control_plane.hpp"
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>
#include <set>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <map>
#include <vector>
#include <memory>

// NEW: Processor state machine
enum class ProcessorState {
    RUNNING = 0,    // Normal operation
    PAUSED = 1,     // Stop consuming, queue grows
    DRAINING = 2    // Finish current work, then pause
};

enum class ProcessState {
    RUNNING = 0,
    STOPPED = 1,
    PAUSED = 2
};

// ... rest of file unchanged
```

---

## 3️⃣ AdminLoop Method Declarations

**Location**: `include/admin/admin_loop.hpp` (ADD TO CLASS)

```cpp
#pragma once 

#include <atomic>
#include <thread>
#include <unordered_map>
#include <spdlog/spdlog.h>

#include "eventprocessor/processManager.hpp"
#include "metrics/metricRegistry.hpp"
#include "control/PipelineState.hpp"
#include "admin/ControlDecision.hpp"  // ADD THIS

class Admin {
public:
    explicit Admin(ProcessManager& pm);
    ~Admin() noexcept;

    void start();
    void stop();

private:
    void loop();
    void reportMetrics(const std::unordered_map<std::string, MetricSnapshot>& snapshots);
    
    void control_tick();
    
    // NEW: Decision logic (pure function)
    ControlDecision evaluateSnapshot(const MetricsSnapshot& snap);
    
    // NEW: Decision execution (multiple actions)
    void executeDecision(const ControlDecision& decision);

    ProcessManager& process_manager_;
    PipelineStateManager pipeline_state_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;

    // Thresholds for decisions
    static constexpr uint64_t CRITICAL_QUEUE_DEPTH = 10000;
    static constexpr uint64_t HIGH_QUEUE_DEPTH = 5000;
    static constexpr double CRITICAL_DROP_RATE = 5.0;   // 5%
    static constexpr double HIGH_DROP_RATE = 2.0;       // 2%
    static constexpr uint64_t CRITICAL_LATENCY_MS = 500;
    static constexpr uint64_t HIGH_LATENCY_MS = 250;
};
```

---

## 4️⃣ evaluateSnapshot() Implementation

**Location**: `src/admin/admin_loop.cpp` (ADD METHOD)

```cpp
ControlDecision Admin::evaluateSnapshot(const MetricsSnapshot& snap) {
    // Pure function: MetricsSnapshot → ControlDecision
    // Can be tested independently without any dependencies
    
    uint64_t queue_depth = snap.current_queue_depth;
    uint64_t latency_ms = snap.get_avg_latency_ns() / 1'000'000;
    double drop_rate = snap.get_drop_rate_percent();
    
    // CRITICAL checks (highest priority)
    if (queue_depth > CRITICAL_QUEUE_DEPTH) {
        return {
            ControlAction::DROP_BATCH,
            FailureState::CRITICAL,
            "Queue depth " + std::to_string(queue_depth) + 
            " exceeds critical limit " + std::to_string(CRITICAL_QUEUE_DEPTH)
        };
    }
    
    if (drop_rate > CRITICAL_DROP_RATE) {
        return {
            ControlAction::PUSH_DLQ,
            FailureState::CRITICAL,
            "Drop rate " + std::to_string(static_cast<int>(drop_rate)) + 
            "% exceeds critical threshold " + std::to_string(static_cast<int>(CRITICAL_DROP_RATE)) + "%"
        };
    }
    
    if (latency_ms > CRITICAL_LATENCY_MS) {
        return {
            ControlAction::DRAIN,
            FailureState::CRITICAL,
            "Latency " + std::to_string(latency_ms) + 
            "ms exceeds critical threshold " + std::to_string(CRITICAL_LATENCY_MS) + "ms"
        };
    }
    
    // DEGRADED checks (medium priority)
    if (queue_depth > HIGH_QUEUE_DEPTH) {
        return {
            ControlAction::PAUSE_PROCESSOR,
            FailureState::DEGRADED,
            "Queue depth " + std::to_string(queue_depth) + 
            " exceeds high watermark " + std::to_string(HIGH_QUEUE_DEPTH)
        };
    }
    
    if (drop_rate > HIGH_DROP_RATE) {
        return {
            ControlAction::PAUSE_PROCESSOR,
            FailureState::DEGRADED,
            "Drop rate " + std::to_string(static_cast<int>(drop_rate)) + 
            "% exceeds threshold " + std::to_string(static_cast<int>(HIGH_DROP_RATE)) + "%"
        };
    }
    
    if (latency_ms > HIGH_LATENCY_MS) {
        return {
            ControlAction::PAUSE_PROCESSOR,
            FailureState::DEGRADED,
            "Latency " + std::to_string(latency_ms) + 
            "ms exceeds threshold " + std::to_string(HIGH_LATENCY_MS) + "ms"
        };
    }
    
    // HEALTHY (no action needed)
    return {
        ControlAction::NONE,
        FailureState::HEALTHY,
        "System healthy - queue=" + std::to_string(queue_depth) + 
        " latency=" + std::to_string(latency_ms) + "ms" +
        " drop_rate=" + std::to_string(static_cast<int>(drop_rate)) + "%"
    };
}
```

---

## 5️⃣ executeDecision() Implementation

**Location**: `src/admin/admin_loop.cpp` (ADD METHOD)

```cpp
void Admin::executeDecision(const ControlDecision& decision) {
    // Execute the formal decision - may take multiple actions
    
    spdlog::warn("[CONTROL DECISION] Action={} Reason={} Details={}",
                 ControlDecision::actionString(decision.action),
                 ControlDecision::failureStateString(decision.reason),
                 decision.details);
    
    switch (decision.action) {
    case ControlAction::NONE:
        spdlog::debug("[EXECUTE] No action - system healthy");
        break;
        
    case ControlAction::PAUSE_PROCESSOR:
        spdlog::warn("[EXECUTE] Pausing TransactionalProcessor");
        process_manager_.pauseTransactions();
        break;
        
    case ControlAction::DRAIN:
        spdlog::warn("[EXECUTE] Draining TransactionalProcessor");
        process_manager_.resumeBatchEvents();  // Finish current batch
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        process_manager_.pauseTransactions();  // Then pause
        break;
        
    case ControlAction::DROP_BATCH:
        spdlog::error("[EXECUTE] Dropping batch from queue");
        process_manager_.dropBatchEvents();
        break;
        
    case ControlAction::PUSH_DLQ:
        spdlog::error("[EXECUTE] Pushing failed events to DLQ");
        // TODO: Get failed events from processor and append to DLQ
        break;
        
    case ControlAction::RESUME:
        spdlog::info("[EXECUTE] Resuming TransactionalProcessor");
        process_manager_.resumeTransactions();
        break;
        
    default:
        spdlog::error("[EXECUTE] Unknown action: {}", 
                      static_cast<int>(decision.action));
        break;
    }
}
```

---

## 6️⃣ Updated control_tick()

**Location**: `src/admin/admin_loop.cpp` (REPLACE EXISTING)

```cpp
void Admin::control_tick() {
    auto& registry = MetricRegistry::getInstance();
    
    // Get current system metrics
    auto agg = registry.getAggregateMetrics();
    
    // Create snapshot for evaluation
    MetricSnapshot snap{};
    snap.current_queue_depth = agg.total_queue_depth;
    snap.total_processing_time_ns = agg.max_processor_latency_ns;
    snap.total_events_dropped = agg.total_events_dropped;
    snap.total_events_processed = agg.total_events_processed;
    
    // STEP 1: Evaluate metrics and make decision (pure function)
    ControlDecision decision = evaluateSnapshot(snap);
    
    // STEP 2: Execute the decision (may have multiple effects)
    executeDecision(decision);
    
    // STEP 3: Update PipelineState based on decision
    PipelineState newState = PipelineState::RUNNING;
    if (decision.action == ControlAction::DROP_BATCH ||
        decision.action == ControlAction::PUSH_DLQ) {
        newState = PipelineState::DROPPING;
    } else if (decision.action == ControlAction::DRAIN) {
        newState = PipelineState::DRAINING;
    } else if (decision.action == ControlAction::PAUSE_PROCESSOR) {
        newState = PipelineState::PAUSED;
    }
    
    pipeline_state_.setState(newState);
}
```

---

## 7️⃣ StorageEngine::appendDLQ()

**Location**: `include/storage_engine/storage_engine.hpp` (ADD METHOD)

```cpp
class StorageEngine {
public:
    // ... existing methods
    
    /**
     * @brief Append dropped events to DLQ log (persistent)
     * @param events Batch of dropped events
     * @param reason Human-readable reason why events were dropped
     */
    void appendDLQ(const std::vector<EventPtr>& events, const std::string& reason);
    
    /**
     * @brief Get DLQ statistics
     */
    struct DLQStats {
        size_t total_dropped;
        size_t total_recovery_attempts;
        std::string last_drop_reason;
        uint64_t last_drop_timestamp_ms;
    };
    
    DLQStats getDLQStats() const;
};
```

**Location**: `src/storage_engine/storage_engine.cpp` (ADD IMPLEMENTATION)

```cpp
void StorageEngine::appendDLQ(const std::vector<EventPtr>& events, 
                              const std::string& reason) {
    if (events.empty()) return;
    
    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);
    
    // Format: [timestamp] DROPPED: id=X topic=Y reason=Z
    for (const auto& evt : events) {
        if (!evt) continue;
        
        std::string dlq_line = fmt::format(
            "[{}] DROPPED: id={} topic={} priority={} reason={}\n",
            timestamp,
            evt->header.id,
            evt->topic,
            static_cast<int>(evt->header.priority),
            reason
        );
        
        // Append to DLQ log file (persistence)
        dlq_file_ << dlq_line;
    }
    
    // Flush to disk for durability
    dlq_file_.flush();
    
    // Update metrics
    metrics_.dlq_total_dropped.fetch_add(events.size(), std::memory_order_relaxed);
    metrics_.dlq_last_reason = reason;
    
    spdlog::warn("[StorageEngine] Appended {} events to DLQ: {}", 
                 events.size(), reason);
}

StorageEngine::DLQStats StorageEngine::getDLQStats() const {
    return {
        metrics_.dlq_total_dropped.load(std::memory_order_relaxed),
        metrics_.dlq_recovery_attempts.load(std::memory_order_relaxed),
        metrics_.dlq_last_reason,
        metrics_.dlq_last_timestamp_ms.load(std::memory_order_relaxed)
    };
}
```

---

## 📝 Summary: Code Locations

| Component | File | Changes |
|-----------|------|---------|
| **ControlDecision** | `include/admin/ControlDecision.hpp` | CREATE |
| **ProcessorState** | `include/eventprocessor/event_processor.hpp` | ADD enum |
| **AdminLoop methods** | `include/admin/admin_loop.hpp` | ADD declarations |
| **evaluateSnapshot()** | `src/admin/admin_loop.cpp` | ADD implementation |
| **executeDecision()** | `src/admin/admin_loop.cpp` | ADD implementation |
| **Updated control_tick()** | `src/admin/admin_loop.cpp` | REPLACE |
| **StorageEngine DLQ** | `include/storage_engine/storage_engine.hpp` | ADD methods |
| **StorageEngine DLQ impl** | `src/storage_engine/storage_engine.cpp` | ADD implementation |

---

## ✅ Implementation Order

1. Create `ControlDecision.hpp`
2. Add `ProcessorState` enum
3. Update `admin_loop.hpp` with declarations
4. Implement `evaluateSnapshot()` in `admin_loop.cpp`
5. Implement `executeDecision()` in `admin_loop.cpp`
6. Update `control_tick()` in `admin_loop.cpp`
7. Add `appendDLQ()` to `StorageEngine`
8. Build and verify

---

## 🧪 Testing Strategy

### Unit Tests
```cpp
// Test pure evaluateSnapshot function
TEST(AdminLoop, EvaluateSnapshot_CriticalQueueDepth) {
    Admin admin(process_manager);
    MetricSnapshot snap;
    snap.current_queue_depth = 12000;  // Over limit
    
    auto decision = admin.evaluateSnapshot(snap);
    
    ASSERT_EQ(decision.action, ControlAction::DROP_BATCH);
    ASSERT_EQ(decision.reason, FailureState::CRITICAL);
    ASSERT_CONTAINS(decision.details, "12000");
}

TEST(AdminLoop, EvaluateSnapshot_Healthy) {
    Admin admin(process_manager);
    MetricSnapshot snap;
    snap.current_queue_depth = 1000;
    snap.total_processing_time_ns = 50'000'000;  // 50ms
    snap.total_events_dropped = 0;
    snap.total_events_processed = 1000;
    
    auto decision = admin.evaluateSnapshot(snap);
    
    ASSERT_EQ(decision.action, ControlAction::NONE);
    ASSERT_EQ(decision.reason, FailureState::HEALTHY);
}
```

### Integration Tests
```cpp
// Test full execution path
TEST(AdminLoop, ExecuteDecision_DropBatch) {
    Admin admin(process_manager);
    ControlDecision decision(
        ControlAction::DROP_BATCH,
        FailureState::CRITICAL,
        "Queue overload"
    );
    
    admin.executeDecision(decision);
    
    // Verify: ProcessManager.dropBatchEvents() was called
    // Verify: Event counts updated
}
```

---

## 🚀 Ready to Implement!

All code snippets are ready. Start with:
1. Create `ControlDecision.hpp`
2. Follow the implementation order above
3. Build after each major change
4. Run tests to verify

**Estimated time**: 2-3 hours coding + 1-2 hours testing
