# 📨 Event Model & Protocol

> Event structure, wire format, và priority routing.

---

## 📦 Event Structure

### Core Types (from code)

```cpp
// File: include/eventstream/core/events/event.hpp
namespace EventStream {

enum struct EventSourceType {
    TCP,        // Network TCP input
    UDP,        // Network UDP input
    FILE,       // File-based input
    INTERNAL,   // Internal system events
    PLUGIN,     // Plugin-generated
    PYTHON,     // Python binding
};

enum struct EventPriority {
    BATCH = 0,      // Lowest - analytics, logging
    LOW = 1,        // Background tasks
    MEDIUM = 2,     // Normal operations (default)
    HIGH = 3,       // User actions, orders
    CRITICAL = 4    // Highest - safety alerts, emergencies
};

struct EventHeader {
    EventSourceType sourceType;  // 4 bytes
    EventPriority priority;      // 4 bytes
    uint32_t id;                 // 4 bytes - unique identifier
    uint64_t timestamp;          // 8 bytes - nanoseconds
    uint32_t body_len;           // 4 bytes
    uint16_t topic_len;          // 2 bytes
    uint32_t crc32;              // 4 bytes - checksum
    // Total: 30 bytes (padded to 32 for alignment)
};

struct Event {
    EventHeader header;
    std::string topic;
    std::vector<uint8_t> body;
    std::unordered_map<std::string, std::string> metadata;
    
    // Latency tracking (Day 37)
    uint64_t dequeue_time_ns{0};
};

using EventPtr = std::shared_ptr<Event>;

// Utility: Get current time in nanoseconds
inline uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

}  // namespace EventStream
```

### Class Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                           Event                                  │
├─────────────────────────────────────────────────────────────────┤
│  + header: EventHeader                                           │
│  + topic: string                                                 │
│  + body: vector<uint8_t>                                         │
│  + metadata: map<string, string>                                 │
│  + dequeue_time_ns: uint64 (latency tracking)                    │
├─────────────────────────────────────────────────────────────────┤
│  + Event()                                                       │
│  + Event(header, topic, body, metadata)                          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ contains
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        EventHeader                               │
├─────────────────────────────────────────────────────────────────┤
│  + sourceType: EventSourceType     (4 bytes)                    │
│  + priority: EventPriority         (4 bytes)                    │
│  + id: uint32                      (4 bytes)                    │
│  + timestamp: uint64               (8 bytes)                    │
│  + body_len: uint32                (4 bytes)                    │
│  + topic_len: uint16               (2 bytes)                    │
│  + crc32: uint32                   (4 bytes)                    │
├─────────────────────────────────────────────────────────────────┤
│  Total: 32 bytes (cache-line friendly)                          │
└─────────────────────────────────────────────────────────────────┘
```

---

## 🎯 Priority Routing

### Queue Selection by Priority

```
                           Event Priority
                                │
                ┌───────────────┼───────────────┐
                │               │               │
                ▼               ▼               ▼
        ┌───────────┐   ┌───────────┐   ┌───────────┐
        │ CRITICAL  │   │  MEDIUM   │   │   LOW     │
        │   HIGH    │   │           │   │  BATCH    │
        └─────┬─────┘   └─────┬─────┘   └─────┬─────┘
              │               │               │
              ▼               ▼               ▼
        ┌───────────┐   ┌───────────┐   ┌───────────┐
        │ REALTIME  │   │TRANSACTION│   │   BATCH   │
        │   Queue   │   │   Queue   │   │   Queue   │
        │   SPSC    │   │   MPSC    │   │   MPSC    │
        │  16384    │   │  65536    │   │  65536    │
        └───────────┘   └───────────┘   └───────────┘
              │               │               │
              ▼               ▼               ▼
        ┌───────────┐   ┌───────────┐   ┌───────────┐
        │ Realtime  │   │ Transact  │   │  Batch    │
        │ Processor │   │ Processor │   │ Processor │
        │ <100µs    │   │ <1ms      │   │ <10ms     │
        └───────────┘   └───────────┘   └───────────┘
```

### Priority Decision Table

| Priority | Queue | SLA | Processor Features |
|----------|-------|-----|-------------------|
| CRITICAL | REALTIME | < 100µs | AlertHandler callback |
| HIGH | REALTIME | < 100µs | AlertHandler callback |
| MEDIUM | TRANSACTIONAL | < 1ms | LockFreeDedup + Retry 3x |
| LOW | BATCH | < 10ms | 5s window aggregation |
| BATCH | BATCH | < 10ms | 5s window aggregation |

### Dispatcher Logic

```cpp
// File: include/eventstream/core/events/dispatcher.hpp
EventBusMulti::QueueId dispatch(EventPriority priority) {
    switch (priority) {
        case EventPriority::CRITICAL:
        case EventPriority::HIGH:
            return EventBusMulti::QueueId::REALTIME;
        
        case EventPriority::MEDIUM:
            return EventBusMulti::QueueId::TRANSACTIONAL;
        
        case EventPriority::LOW:
        case EventPriority::BATCH:
        default:
            return EventBusMulti::QueueId::BATCH;
    }
}
```

---

## 📡 Wire Protocol

### Frame Format (TCP/UDP)

```
┌────────────────────────────────────────────────────────────────────┐
│                         BINARY FRAME                                │
├────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────┬──────────────┬──────────────┬──────────────────┐ │
│  │    Length    │   Priority   │  Topic Len   │                  │ │
│  │   (4 bytes)  │   (1 byte)   │  (2 bytes)   │                  │ │
│  │  Big-endian  │   uint8      │  Big-endian  │                  │ │
│  └──────────────┴──────────────┴──────────────┘                  │ │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │                      Topic (variable)                        │  │
│  │                      UTF-8 string                            │  │
│  │                      Length: topic_len bytes                 │  │
│  └─────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │                     Payload (variable)                       │  │
│  │                     Binary data                              │  │
│  │            Length: frame_len - 3 - topic_len bytes           │  │
│  └─────────────────────────────────────────────────────────────┘  │
│                                                                     │
└────────────────────────────────────────────────────────────────────┘
```

### Byte Layout

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│                       Frame Length (32-bit BE)                │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│ Priority  │         Topic Length (16-bit BE)        │ Topic.. │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│                    ...Topic (continued)...                    │
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│                         Payload...                            │
└─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘
```

### Example Frame

```
Example: Topic="sensors/temp", Payload='{"val":23.5}', Priority=HIGH (3)

Hex dump:
00 00 00 19    # Frame Length: 25 bytes (1+2+12+12 = 27, excluding length field = 23)
03             # Priority: 3 (HIGH)
00 0C          # Topic Length: 12 bytes
73 65 6E 73 6F 72 73 2F 74 65 6D 70   # "sensors/temp" (UTF-8)
7B 22 76 61 6C 22 3A 32 33 2E 35 7D   # '{"val":23.5}' (JSON payload)

Parsing:
1. Read 4 bytes → frame_len = 25
2. Read 1 byte → priority = 3 (HIGH) → REALTIME queue
3. Read 2 bytes → topic_len = 12
4. Read 12 bytes → topic = "sensors/temp"
5. Read remaining → payload = '{"val":23.5}'
```

---

## 🔄 Event Lifecycle

### Creation → Processing → Storage

```
┌───────────┐    ┌───────────┐    ┌───────────┐    ┌───────────┐
│  Ingest   │    │   Pool    │    │   Queue   │    │ Processor │
└─────┬─────┘    └─────┬─────┘    └─────┬─────┘    └─────┬─────┘
      │                │                │                │
      │  1. acquire()  │                │                │
      │───────────────►│                │                │
      │   EventPtr     │                │                │
      │◄───────────────│                │                │
      │                │                │                │
      │  2. Parse frame, fill event    │                │
      │────────────────────────────────►│                │
      │                │                │                │
      │  3. push(QueueId, event)       │                │
      │────────────────────────────────►│                │
      │                │                │                │
      │                │                │  4. pop()      │
      │                │                │───────────────►│
      │                │                │   EventPtr     │
      │                │                │◄───────────────│
      │                │                │                │
      │                │                │  5. process()  │
      │                │                │───────────────►│
      │                │                │                │
      │                │                │  6. store()    │
      │                │                │───────────────►│
      │                │                │                │
      │  7. Event auto-released via shared_ptr          │
      │◄────────────────────────────────────────────────│
```

### Latency Tracking

```cpp
// When dequeuing (in processor)
event->dequeue_time_ns = nowNs();

// Calculate latency
uint64_t enqueue_time = event->header.timestamp;
uint64_t dequeue_time = event->dequeue_time_ns;
uint64_t queue_latency_ns = dequeue_time - enqueue_time;
```

---

## 📦 DeadLetterQueue

### Purpose

DLQ lưu trữ các events bị drop do backpressure hoặc processing failure.

```cpp
// File: include/eventstream/core/events/dead_letter_queue.hpp
namespace EventStream {

class DeadLetterQueue {
public:
    void push(const EventPtr& event, const std::string& reason);
    void pushBatch(const std::vector<EventPtr>& events, const std::string& reason);
    
    std::optional<std::pair<EventPtr, std::string>> pop();
    
    size_t size() const;
    bool empty() const;
    
    // Statistics
    size_t totalDropped() const;
    std::string lastDropReason() const;
};

}  // namespace EventStream
```

### Drop Reasons

| Reason | Description |
|--------|-------------|
| `queue_full` | Queue at capacity, event dropped |
| `backpressure_drop` | Backpressure activated, batch dropped |
| `processing_failed` | Processor failed after retries |
| `dedup_expired` | Event too old for idempotency window |
| `emergency_drop` | Emergency state, non-critical dropped |

---

## 🎯 Topic Table

### Purpose

Topic table maps topic strings to metadata for routing và filtering.

```cpp
// File: include/eventstream/core/events/topic_table.hpp
namespace EventStream {

class TopicTable {
public:
    struct TopicInfo {
        std::string pattern;          // Topic pattern (supports wildcards)
        EventPriority default_priority;
        std::vector<std::string> tags;
    };
    
    void registerTopic(const std::string& pattern, const TopicInfo& info);
    std::optional<TopicInfo> lookup(const std::string& topic) const;
    
    // Wildcard matching
    bool matches(const std::string& pattern, const std::string& topic) const;
};

}  // namespace EventStream
```

### Topic Patterns

```
# Exact match
sensors/temp              → matches "sensors/temp" only

# Single-level wildcard (+)
sensors/+/temp            → matches "sensors/room1/temp", "sensors/room2/temp"

# Multi-level wildcard (#)
sensors/#                 → matches "sensors/temp", "sensors/room1/temp/avg"
```

---

## ➡️ Next

- [Architecture Overview →](architecture.md)
