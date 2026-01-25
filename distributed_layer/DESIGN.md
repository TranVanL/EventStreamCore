# Distributed Layer Architecture & Design

**Document**: Complete system design for multi-node EventStreamCore  
**Date**: Day 40  
**Status**: Design Phase  
**Audience**: Architect (yourself), future code reviewers  

---

## PART 1: System Overview

### Current State (Single Node)
```
TCP Ingest
    ↓
Event Pool (pre-alloc 65K)
    ↓
EventBus (dispatcher)
    ↓ (lock-free SPSC)
3 Processors (Realtime/Transactional/Batch)
    ↓
Deduplicator (lock-free, 4096 buckets)
    ↓
Storage Engine (RocksDB)
```

**Performance**: 82K events/sec, p99 < 5ms  
**CPU**: 85% utilized (15% spare)  
**Memory**: 100-130 MB

### Target State (3-Node Cluster)
```
        Leader EventStreamCore
        ├─ TCP Ingest (receives events)
        ├─ EventBus + Processors
        ├─ Local Dedup
        └─ Replication Thread
            ├─→ SPSC to Follower 1 ───→ Replica 1
            └─→ SPSC to Follower 2 ───→ Replica 2

        Follower 1 EventStreamCore
        ├─ Receives replicated events
        ├─ Processes locally (read-only)
        ├─ Local Dedup (consistency)
        └─ Storage (hot standby)

        Follower 2 EventStreamCore
        ├─ Receives replicated events
        ├─ Processes locally (read-only)
        ├─ Local Dedup (consistency)
        └─ Storage (hot standby)
```

**Design**: Each node runs full EventStreamCore, leader accepts writes, followers accept reads + standby.

---

## PART 2: Architecture Components

### A. Replication Protocol

#### Event Batching Strategy
```
Leader EventStreamCore:
  events → replication_queue (SPSC, lock-free)
            ↓ (background thread)
            collect_batch()
              - Accumulate up to 64 events
              - OR wait max 1ms timeout
              - Whichever comes first
            ↓
            serialize_batch()
              - Pack: [batch_header][event1][event2]...[event64]
              - Header: magic, batch_id, event_count, timestamp
            ↓
            send_to_followers()
              - Send via dedicated TCP socket
              - Async (don't block on ack)
              - Retry on failure
            ↓
            wait_for_ack()
              - Follower 1 acks: "got batch #123"
              - Follower 2 acks: "got batch #123"
              - Once both ack: mark as replicated
```

**Rationale**:
- **64 events**: Fits in ~32KB batch (typical event = 512B)
- **1ms timeout**: Ensures max latency even at low throughput
- **Lock-free SPSC**: Proved pattern, no contention
- **Async sending**: Leader doesn't block on replication

#### Serialization Format
```
BATCH_MESSAGE:
┌─────────────────────────────────────────┐
│ HEADER (48 bytes):                      │
│  - magic: u32 = 0xDEADBEEF              │
│  - version: u8 = 1                      │
│  - batch_id: u64 (monotonic per leader) │
│  - event_count: u16 (1-64)              │
│  - timestamp_ms: u64 (leader time)      │
│  - checksum: u32 (CRC32 of payload)     │
├─────────────────────────────────────────┤
│ EVENTS (variable, 512B - 32KB):         │
│  [event1] [event2] ... [eventN]         │
│  Each event: [header] [type] [payload]  │
│  Already serialized from EventFactory   │
├─────────────────────────────────────────┤
│ FOOTER (8 bytes):                       │
│  - end_marker: u64 = 0x01DEADBEEF      │
└─────────────────────────────────────────┘
```

**Parsing on Follower**:
```cpp
struct BatchMessage {
    uint32_t magic;
    uint8_t version;
    uint64_t batch_id;
    uint16_t event_count;
    uint64_t timestamp_ms;
    uint32_t checksum;
    std::vector<Event> events;  // parsed on arrival
    uint64_t end_marker;
};

// On receiver:
batch = deserialize_batch(buffer);
if (batch.magic != 0xDEADBEEF) throw;  // corrupt
if (!verify_checksum(batch)) throw;    // checksum failed
process_events(batch.events);           // normal processing
ack_to_leader(batch.batch_id);          // send ack
```

### B. RAFT Consensus Layer

#### What RAFT Does (For Distributed Dedup & Failover)
```
RAFT Log (replicated across all 3 nodes):
┌─────────────────────────────────────────┐
│ Term 1:                                 │
│  Index 1: Dedup entry (event_id=123)   │
│  Index 2: Dedup entry (event_id=456)   │
│  Index 3: Config change (add Follower2) │
├─────────────────────────────────────────┤
│ Term 2:  (new leader elected)           │
│  Index 4: Dedup entry (event_id=789)   │
│  Index 5: ...                           │
└─────────────────────────────────────────┘
```

**Not Used For**:
- ❌ Event replication (async batching is better)
- ❌ Normal event processing (too slow, log-per-event overhead)

**Used For**:
- ✅ Deduplication state consistency
- ✅ Leader election
- ✅ Cluster membership changes
- ✅ State recovery after failure

#### Leader Election (Simplified)
```
SCENARIO: Leader crashes

Followers detect: 3 missed heartbeats (450ms)
                ↓
Follower 1: "I'm candidate, voting for myself"
Follower 2: "I'm candidate, voting for myself"
                ↓ (election timeout 50-150ms)
One wins (higher term/log index)
                ↓
Winner becomes Leader
                ↓
Sends heartbeat: "I'm the new leader"
Other follower: "Ok, I'm follower now"
                ↓
Total time: ~450ms (detection) + 150ms (election) = ~600ms
```

**During Election**:
- ❌ Leader DOES NOT accept new events (blocks)
- ✅ Followers ACCEPT new events (buffered locally)
- ✅ Old events still being processed
- ✅ After election, new leader sends buffered events

### C. Distributed Deduplication

#### Per-Node Dedup (Local)
Each node maintains own dedup table:
```
Node 1 (Leader):
  Dedup table: {event_id → last_seen_time}
  - Sees event 123, stores it
  - Replicates event 123 to followers

Node 2 (Follower 1):
  Dedup table: {event_id → last_seen_time}
  - Receives event 123 from replication
  - Stores it locally
  - Processes normally

Node 3 (Follower 2):
  Dedup table: {event_id → last_seen_time}
  - Receives event 123 from replication
  - Stores it locally
  - Processes normally
```

#### Consistency Guarantee
```
IF a unique event_id replicates to all nodes
THEN exactly one processing per node (idempotency preserved)

Why?
1. Leader dedup table has event_id
2. Replication sends event to followers
3. Followers receive same event_id
4. Followers' dedup tables prevent re-processing
5. Result: 3 copies, but processed exactly once per node
```

#### TTL-Based Cleanup (Distributed)
```
Dedup entry: {event_id: 123, timestamp: t0}

After TTL (default 24 hours):
- Each node independently removes old entries
- No coordination needed (eventual consistency)
- If same event arrives after TTL: reprocessed (acceptable edge case)
```

### D. Failover Mechanism

#### Detection Phase (450ms)
```
Leader ──heartbeat──→ Follower 1
                     (checks: time < last_hb + 150ms)

If NO heartbeat for 3 intervals:
  Follower 1 thinks: "Leader is dead"
  (450ms = 3 × 150ms)
```

#### Election Phase (150ms)
```
Follower 1: RequestVote RPC to Follower 2
            "Vote for me as leader"
            
Follower 2: Check log state, vote yes/no
            Reply: "I vote for you"

Follower 1 receives 2 votes (self + other):
            "I'm leader now!"
            
Followers 2 discovers new term:
            "I'm follower now"
```

#### Recovery Phase (< 1 second)
```
Old Leader (was down): Rejoins cluster
Old Leader sees: "Term is higher than mine"
Old Leader: "I'm follower now, catch me up"
Other nodes: Send it the RAFT log
Old Leader: Replays log, updates storage
```

**Total Failover Time**: ~600ms worst case

#### No Data Loss Guarantee
```
Event accepted on leader
↓
Replicated to followers (async)
↓
Followers ack: "Got batch #5"
↓
Leader commits batch #5 (2/3 acks)
↓
Leader can now safely say: "Event is committed"

IF leader crashes after commit:
- Followers have the batch (already replicated)
- New leader continues from their log
- Zero loss

IF leader crashes BEFORE commit:
- Event not replicated
- New leader doesn't have it
- Client can retry (idempotency via dedup)
```

---

## PART 3: Threading Model

### Replication Thread
```
EventStreamCore (Main):
├─ TCP ingest thread (existing)
├─ EventBus dispatcher (existing)
├─ 3 Processors (existing)
├─ Dedup cleaner (existing)
│
└─ Replication Manager (NEW)
   ├─ SPSC queue from dispatcher
   │ (receives events to replicate)
   │
   ├─ Batch accumulator (background)
   │ - Waits for 64 events or 1ms
   │ - Calls serialize()
   │ - Enqueues to network thread
   │
   ├─ Network sender (background)
   │ - Reads batches
   │ - Sends to Follower 1 (async)
   │ - Sends to Follower 2 (async)
   │ - Waits for ack
   │ - Updates commit index
   │
   ├─ RAFT state machine
   │ - Leader election
   │ - Term tracking
   │ - Heartbeat generation (every 100ms)
   │ - Vote handling
   │
   └─ RAFT log manager
     - In-memory log (for throughput)
     - Persistent log (RocksDB)
     - Commit point tracking
```

### Thread Count
```
Core Engine: 5-6 threads
├─ Main thread (coordination)
├─ TCP accept thread
├─ Event pool cleaner
├─ Metrics aggregator
└─ 1-2 processor threads (configurable)

Replication: 2-3 NEW threads
├─ Batch accumulator
├─ Network sender (to followers)
└─ Heartbeat responder

Total: 7-9 threads (light for 3-node system)
```

### Lock-Free Pattern
```
Dispatcher → Replication SPSC queue (lock-free)
                ↓
            Batch accumulator (reads from queue)
                ↓
            Network sender (sends to followers)

No locks involved in hot path!
Only atomic operations:
- SPSC queue (existing pattern)
- Commit index update (atomic u64)
- Current term (atomic u64)
```

---

## PART 4: Configuration & Initialization

### Cluster Topology
```yaml
cluster:
  node_id: 1              # 0, 1, 2
  is_leader: true         # Manual or auto-detect
  nodes:
    - id: 0
      host: localhost
      port: 9001
    - id: 1
      host: localhost
      port: 9002
    - id: 2
      host: localhost
      port: 9003
```

### Startup Sequence
```
1. Load configuration
   └─ Read cluster.yaml, node_id, peers

2. Start core engine
   └─ TCP ingest, processors, dedup, storage

3. Start RAFT subsystem
   └─ Initialize state (term, voted_for, log)
   └─ Load persistent log from RocksDB
   └─ Start heartbeat responder thread

4. Assume role
   └─ If configured as leader: become leader
   └─ If configured as follower: wait for heartbeat
   └─ If no heartbeat: start election

5. Start replication
   └─ If leader: spawn network sender threads
   └─ If follower: spawn heartbeat responder
```

### Graceful Shutdown
```
1. Stop accepting new events
2. Wait for in-flight events to replicate
3. Close RAFT connections
4. Flush storage
5. Exit
```

---

## PART 5: Performance Implications

### Throughput Impact
```
Single node baseline: 82K events/sec
3-node with replication: 60-70K events/sec
Drop reason:
  - Serialization (1-2K events/sec)
  - Network send + ack (2-5K events/sec)
  - RAFT overhead (2K events/sec)
  Total: ~12K events/sec drop (15%)

Acceptable? Yes:
  - Still 60K+ is high throughput
  - Replication cost for 3-way safety
```

### Latency Impact
```
Single node: p99 = 5ms
3-node:      p99 = 10-15ms

Added latency sources:
  - Network roundtrip: 5-10ms
  - Follower processing: 1-2ms
  - Leader waiting for ack: 2-3ms (usually)

But: Still sub-20ms for p99, excellent for event systems
```

### CPU Impact
```
Leader (3 processes = 85% CPU):
  - Core engine: ~75%
  - Replication thread: ~10%
  Total: 85-90% (still has 10-15% spare)

Followers (2 processes = 85% CPU):
  - Core engine: ~82%
  - Heartbeat responder: ~2%
  Total: 85-87%
```

### Memory Impact
```
Per node: +50-70MB for replication state
  - RAFT log: 20-30MB (stores metadata)
  - Network buffers: 10-20MB
  - Batch accumulator: 5-10MB
  - Commit state: <1MB

Total per node: 150-200MB (was 100-130MB)
Acceptable: Yes (still single-digit percentage of modern systems)
```

---

## PART 6: Edge Cases & Design Decisions

### Edge Case 1: Network Partition
```
Scenario: Leader isolated from both followers
  Leader thinks: "I'm still leader"
  Followers think: "Leader is dead"
  Followers elect new leader from themselves
  
Result: Two leaders (split brain!)

Prevention: Quorum voting
  Leader must get ack from majority (2 of 3)
  Before committing event
  
  If partitioned:
    - Old leader: has no majority, can't commit
    - New leader: has majority, continues
    
Why it works:
  - At most one partition has majority
  - Only majority leader can commit
  - No conflicts possible
```

### Edge Case 2: Slow Follower
```
Scenario: Follower 2 is slow (network lag)
  Leader: Sends batch to Follower 1 (fast)
  Leader: Sends batch to Follower 2 (slow)
  
  Follower 1: Acks quickly
  Follower 2: Acks slowly (30s lag)

Design: Async replication
  Leader: Doesn't wait for Follower 2
  Leader: After Follower 1 acks, mark as committed
  Leader: Can accept new events
  
  Follower 2: Still processing batch asynchronously
  
Result: Throughput not affected by slow follower
```

### Edge Case 3: Duplicate Detection After Restart
```
Scenario: Follower 2 crashes, recovers
  Follower 2: Lost last batch in memory
  Follower 2: Reads RAFT log from persistent storage
  Follower 2: Replays log: "Batches 1-100 committed"
  Follower 2: Dedup table: has event IDs from batches 1-99
  Follower 2: Missing batch 100 from dedup table
  
  Leader: Resends batch 100
  Follower 2: Dedup table: "don't have event_id_xyz"
  Follower 2: Processes again (but dedup prevents duplicate processing!)

Result: Automatic recovery without data loss
```

### Edge Case 4: Leader Promotion During Replication
```
Scenario: Follower becomes new leader mid-replication
  Old Leader: Crashes
  Follower 1: Elected as new leader
  Follower 2: Follower
  
  Follower 1 (now Leader): Reads stored log
  Follower 1: "I have batches 1-95 committed, 96-100 uncommitted"
  Follower 1: Can start accepting new events on batch 101
  Follower 1: Eventually replicates to Follower 2
  
  Uncommitted batch 96-100: Eventually replicated or discarded
  (Depends on when original leader crashed)

Guarantee: No loss of committed events
```

---

## PART 7: Design Rationale (Why These Choices?)

### Why Async Replication?
```
Sync replication (wait for all followers):
  ❌ Slow (network RTT × 2 = 10-20ms per event)
  ❌ P99 latency spikes
  ❌ Throughput cut to ~40K events/sec

Async replication (send, don't wait):
  ✅ Fast (log locally first)
  ✅ P99 latency < 15ms
  ✅ Throughput 60-70K events/sec
  ✅ Still consistent (RAFT ensures delivery)
```

### Why Batching (64/1ms)?
```
Per-event replication:
  ❌ 64 network sends per batch window
  ❌ 64 serialization operations
  ❌ Overhead huge

Batching 64 events:
  ✅ 1 network send per 64 events
  ✅ 1 deserialization per batch
  ✅ 3-4x throughput improvement
  ✅ Max latency: 1ms (good enough)
```

### Why Eventual Consistency for Dedup?
```
Strong consistency (sync dedup across all nodes):
  ❌ Requires coordination on every event
  ❌ Distributed transaction for each event
  ❌ Very slow

Eventual consistency (dedup replicated):
  ✅ Each node decides independently
  ✅ Dedup state replicates with events
  ✅ Fast, simple, works with async replication
  ✅ Edge case: rare duplicates if node crashes mid-processing
     But dedup prevents actual duplicate processing!
```

### Why 3 Nodes (Not 5 or 7)?
```
3 nodes:
  - Fault tolerance: can lose 1 node (2 > 3/2)
  - Cost: minimal (3 copies of data)
  - Complexity: manageable
  - Quorum: 2 of 3 is simple majority

5 nodes:
  ❌ Cost: 5x data storage
  ❌ Complexity: more nodes to manage
  ❌ Overkill for most use cases

7+ nodes:
  ❌ Distributed consensus becomes complex
  ❌ Network coordination overhead
  ❌ For EventStreamCore scope: unnecessary
```

### Why RAFT (Not Paxos / Other Consensus)?
```
RAFT (chosen):
  ✅ Single leader model (simple)
  ✅ Already partially implemented in core
  ✅ Proven (used in etcd, Consul, etc.)
  ✅ Easy to understand and debug

Paxos:
  ❌ Multi-leader (complex)
  ❌ Hard to understand
  ❌ Harder to debug

Custom:
  ❌ Distributed consensus is hard
  ❌ Lots of edge cases
  ❌ RAFT is battle-tested
```

---

## PART 8: Integration with Core Engine

### What Core Engine Provides
```
✅ Lock-free SPSC queues
   → Used for replication queues

✅ Event pool (pre-allocated)
   → Replication reuses same pool

✅ EventBus dispatcher
   → Outputs events to replication SPSC

✅ RAFT state machine skeleton
   → Extended for full consensus

✅ Storage engine (RocksDB)
   → Stores RAFT log persistently

✅ Deduplicator (lock-free)
   → Each node has own, replicated
```

### Integration Points
```
Core Engine                 Replication Layer
   │                              │
   Dispatcher ────SPSC───→ Replication queue
   │                              │
   Storage ←─────────────── RAFT log
   │                              │
   Dedup ←────Event ID──── Distributed dedup
   │                              │
   Processors ←─────── Replicated events
```

### No Core Changes Required
```
✅ EventBusMulti: Already outputs to SPSC
✅ Event pool: No changes needed
✅ Dedup: Can be extended, not replaced
✅ Storage: Already has RocksDB for RAFT
✅ Processor: No changes (processes same events)

Approach: Build replication as ADD-ON, not replacement
```

---

## Summary

### What Distributed Layer Does
1. **Replicates events** from leader to followers (async batching)
2. **Maintains consensus** via RAFT (leader election + log)
3. **Guarantees idempotency** via distributed dedup
4. **Detects failures** via heartbeat (450ms detection)
5. **Recovers automatically** via RAFT log replay

### Performance
- Throughput: 60-70K events/sec (15% overhead acceptable)
- Latency: p99 < 15ms (network + consensus)
- CPU: 85-90% (still room for headroom)
- Memory: 150-200MB per node (acceptable)

### Complexity
- Code size: ~1000 lines of C++ (replication, RAFT extensions, network)
- New components: 4 main (replication, RAFT, failover, dedup coordinator)
- Thread count: +2-3 new threads
- Testing: 30+ unit tests + 10+ integration scenarios

### Timeline
- Week 40: Protocol + design complete
- Week 41: RAFT + replication core
- Week 42: Failover + comprehensive testing

---

**Next Document**: PROTOCOL.md (serialization details)  
**Then**: IMPLEMENTATION_PLAN.md (week-by-week tasks)

