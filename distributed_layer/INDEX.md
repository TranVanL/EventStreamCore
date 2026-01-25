# Distributed Layer Documentation Index

**Status**: Day 40 - Design Phase  
**Core Philosophy**: Event replication + RAFT consensus + automatic failover  
**Timeline**: Week 40-42 (3 weeks)  
**Team**: Solo developer, lock-free systems specialist

---

## Overview

This folder contains all documentation for **distributed layer** of EventStreamCore.

**Goal**: Transform single-node event engine into **multi-node distributed system** with:
- ✅ Event replication (3-way replication)
- ✅ RAFT consensus (leader election + log)
- ✅ Distributed deduplication (consistent hashing)
- ✅ Automatic failover (< 150ms detection)
- ✅ Zero loss of idempotency guarantees

**Architecture**: Distributed consensus layer built ON TOP of proven single-node core engine.

---

## Quick Navigation

### 📘 Architecture & Design
1. **DESIGN.md** (START HERE)
   - Complete system architecture
   - 3-node cluster model
   - Event flow (leader → followers)
   - Replication guarantees

2. **PROTOCOL.md**
   - Event replication protocol
   - Batching strategy (64 events / 1ms)
   - Serialization format
   - Network reliability

3. **FAILOVER.md**
   - Leader detection mechanism
   - Automatic promotion logic
   - State recovery procedure
   - Split-brain prevention

### 🔧 Implementation Details
4. **IMPLEMENTATION_PLAN.md**
   - Week 40: What to code
   - Week 41: What to code
   - Week 42: What to code
   - Testing strategy per week

5. **CODE_STRUCTURE.md**
   - New files to create
   - Integration points with core engine
   - Thread model for replication
   - Performance implications

### 📊 Supporting Docs
6. **PERFORMANCE_TARGETS.md**
   - Throughput targets (60-70K events/sec)
   - Latency impact (p99 ~15ms)
   - CPU overhead (5-10%)
   - Memory overhead (per node)

7. **TESTING_STRATEGY.md**
   - Unit tests (RAFT log replication)
   - Integration tests (3-node cluster)
   - Chaos tests (network failures, node crashes)
   - Benchmark suite

8. **DEPLOYMENT_GUIDE.md**
   - How to run 3-node cluster
   - Configuration (node IDs, peers)
   - Monitoring & health checks
   - Operations playbooks

### 🎨 Visual Diagrams
9. **SEQUENCE_DIAGRAMS.md**
   - Normal flow (3-node replication)
   - Leader election flow
   - Failover scenario
   - State recovery timeline
   - Network partition handling

---

## Reading Path by Role

### **For Yourself (Architect + Implementer)**
Start → Finish:
1. DESIGN.md (30 min) - Understand overall system
2. PROTOCOL.md (20 min) - Understand serialization
3. IMPLEMENTATION_PLAN.md (15 min) - Know what to code each week
4. CODE_STRUCTURE.md (15 min) - Know where code goes
5. FAILOVER.md (15 min) - Understand edge cases
6. TESTING_STRATEGY.md (20 min) - Know how to validate

**Total**: 2 hours to understand everything before coding

### **For Code Reviewer (Hypothetical)**
1. DESIGN.md (architecture overview)
2. PROTOCOL.md (serialization details)
3. CODE_STRUCTURE.md (where changes are)
4. IMPLEMENTATION_PLAN.md (what's expected per week)
5. Source code + inline comments

### **For Operations Person**
1. DESIGN.md (system overview, 10 min)
2. DEPLOYMENT_GUIDE.md (how to run it)
3. TESTING_STRATEGY.md (verification steps)
4. Monitoring section in PERFORMANCE_TARGETS.md

---

## Key Design Decisions (TL;DR)

### 1. **Event Replication Model**
- **Batching**: 64 events OR 1ms timeout (whichever first)
- **Guarantees**: At-least-once + dedup → exactly-once
- **Direction**: Leader pushes to followers asynchronously
- **Acks**: Followers ack to leader after persistent storage

### 2. **RAFT Configuration**
- **Cluster Size**: 3 nodes (leader + 2 followers)
- **Term Duration**: 150ms heartbeat interval
- **Log Storage**: In-memory log + persistence to disk
- **Commit Point**: Leader commits when 2/3 replicas ack

### 3. **Deduplication Distribution**
- **Local Dedup**: Each node has own dedup table
- **Replication**: Dedup events replicate with other events
- **Consistency**: Eventual consistency across cluster
- **Cleanup**: Distributed TTL-based cleanup

### 4. **Failover Strategy**
- **Detection**: Missing 3 heartbeats = 450ms timeout
- **Election**: RAFT election (takes ~150ms)
- **Promotion**: New leader immediately starts accepting events
- **Recovery**: Followers replay from distributed log

### 5. **Network I/O**
- **Per-Node Connection**: Dedicated TCP socket per replication pair
- **Async Pattern**: SPSC queue between replication thread and socket
- **Backpressure**: Replication blocks if follower can't keep up
- **Timeout**: 5s per operation, auto-retry

---

## Current Status

### ✅ Completed (Core Engine Day 39)
- Single-node event engine (82K events/sec)
- Lock-free SPSC queues
- Event pool (pre-allocated)
- Lock-free deduplicator
- RAFT state machine (not yet used)
- Async metrics
- Zero-copy TCP ingest

### 🏗️ In Progress (Day 40+)
- **Week 40**: Replication protocol design + SPSC between nodes
- **Week 41**: RAFT leader election + log replication
- **Week 42**: Failover + distributed dedup + testing

### ⏳ Future (Post-Day 42)
- REST API (minimal, for operations)
- Web dashboard (simple monitoring)
- Deployment automation
- Production hardening

---

## Technology Stack

**Language**: C++ (C++17, already used in core)  
**Concurrency**: Lock-free + RAFT consensus  
**Network**: TCP sockets (same as TCP ingest)  
**Storage**: RocksDB (already used)  
**Testing**: Google Test (already in use)  

---

## Success Criteria

### Week 40 (Protocol Design)
- ✅ Replication protocol documented
- ✅ Serialization format defined
- ✅ SPSC queue for replication threads designed
- ✅ Network reliability plan written

### Week 41 (RAFT Implementation)
- ✅ Leader election working
- ✅ Log replication across cluster
- ✅ All 3 nodes replicate events correctly
- ✅ Tests: 20+ tests covering RAFT logic

### Week 42 (Failover + Integration)
- ✅ Leader failure → automatic promotion (< 150ms)
- ✅ Distributed dedup prevents duplicates across cluster
- ✅ 3-node cluster handles all scenarios
- ✅ Performance: 60-70K events/sec, p99 < 15ms

---

## File Organization

```
distributed_layer/
├── INDEX.md                           (THIS FILE - Navigation)
│
├── DESIGN.md                          (Week 40: Architecture)
├── PROTOCOL.md                        (Week 40: Serialization)
├── FAILOVER.md                        (Week 40: Recovery)
│
├── IMPLEMENTATION_PLAN.md             (Week 40-42: Week-by-week)
├── CODE_STRUCTURE.md                  (Week 40-42: Where code goes)
├── PERFORMANCE_TARGETS.md             (Week 40-42: Benchmarks)
├── TESTING_STRATEGY.md                (Week 40-42: Validation)
│
├── SEQUENCE_DIAGRAMS.md               (Week 40: Visual flows)
├── DEPLOYMENT_GUIDE.md                (Week 42: Operations)
│
└── NOTES/                             (Dev notes, ephemeral)
    └── (Daily progress notes - can delete after week)
```

---

## How to Use This Folder

### Starting Implementation
1. Read this INDEX.md (5 min) ← You are here
2. Read DESIGN.md (30 min)
3. Read IMPLEMENTATION_PLAN.md (15 min)
4. Start coding Week 40 tasks

### During Implementation
- Reference PROTOCOL.md for serialization details
- Reference CODE_STRUCTURE.md for where to put code
- Reference TESTING_STRATEGY.md for validation
- Write daily notes in NOTES/ folder

### Weekly Reviews
- After Week 40: Review DESIGN.md completeness
- After Week 41: Review PROTOCOL.md + implementation match
- After Week 42: Run TESTING_STRATEGY.md checklist

---

## Key Metrics to Track

### Throughput
- Single node: 82K events/sec (baseline)
- 3-node with replication: Target 60-70K events/sec
- Acceptable drop: 15-20% (due to replication overhead)

### Latency
- p50 (median): < 5ms (at 60K events/sec)
- p99 (tail): < 15ms (network + consensus overhead)
- p999: < 50ms (worst case)

### CPU
- Replication thread: ~5-10% additional CPU
- Total on leader: 90-95% (still has headroom)
- Total on followers: 85-90%

### Memory
- Per node: 150-200MB (130MB core + 20-70MB replication state)
- RAFT log: ~100KB per node (stores event metadata)

---

## Common Questions

### Q: Why 3 nodes?
A: Minimum for fault tolerance. 1 leader + 2 followers = can lose 1 node and still operate.

### Q: Why 64 events per batch?
A: Balances latency (1ms max) vs throughput (batches reduce syscalls).

### Q: Why async replication?
A: Leader doesn't block on replication, maintains high throughput.

### Q: What about consistency?
A: Eventual consistency during normal ops. Strong consistency on reads via leader.

### Q: How long failover takes?
A: Detection (450ms) + election (150ms) = ~600ms worst case.

### Q: Can I add more nodes?
A: Design is for 3 nodes. Larger cluster needs quorum analysis (see DESIGN.md).

---

## Next Steps

### Right Now (You've read INDEX.md)
1. Read DESIGN.md (30 min)
2. Read IMPLEMENTATION_PLAN.md (15 min)
3. Create initial code structure (20 min)

### Tomorrow (Week 40 Day 1)
1. Finalize serialization format
2. Create replication queue classes
3. Write first tests

### Timeline
- **Week 40**: Design phase complete, basic replication framework
- **Week 41**: RAFT fully integrated, 3-node cluster working
- **Week 42**: Failover tested, distributed dedup working, full testing

---

## Status Updates Location

Progress will be tracked in:
- Daily notes: `distributed_layer/NOTES/day_40.md`, `day_41.md`, etc.
- This INDEX.md will be updated with completion status
- Full status in parent directory: `DAY40_PROGRESS.md` (root level)

---

**Ready to start? Read DESIGN.md next →**

