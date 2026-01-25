# Implementation Plan - Week 40-42

**Document**: Week-by-week breakdown of distributed layer implementation  
**Timeline**: Week 40 (Design) → Week 41 (Core) → Week 42 (Testing)  
**Effort**: ~3 weeks, solo developer, 40-50 hours per week  

---

## WEEK 40: Architecture & Design Foundation

**Goal**: All design complete, no code yet  
**Output**: Complete documentation + code scaffolding  
**Time**: 40-50 hours  

### Week 40, Day 1 (Monday)
**Theme**: Understand DESIGN.md deeply

```
9:00-10:00   Review DESIGN.md
             - 3-node architecture
             - Replication flow
             - RAFT integration

10:00-11:30  Analyze core engine
             - Review EventBusMulti
             - Review SPSC queue pattern
             - Review existing RAFT skeleton

11:30-12:00  Create distributed_layer/PROTOCOL.md
             - Serialization format
             - Batching strategy
             - Network reliability
```

**Deliverables**:
- ✅ DESIGN.md (completed)
- 🔄 PROTOCOL.md (in progress)

### Week 40, Day 2 (Tuesday)
**Theme**: Protocol specification

```
9:00-11:00   Finalize PROTOCOL.md
             - Batch message format (binary spec)
             - Serialization examples
             - Ack protocol

11:00-12:00  Create CODE_STRUCTURE.md
             - New files to create
             - Where replication threads fit
             - Thread spawning model
             - Integration with Event

12:00-13:00  Sketch IMPLEMENTATION_PLAN.md (this file)
             - Week-by-week breakdown
             - Daily tasks
             - Validation steps
```

**Deliverables**:
- ✅ PROTOCOL.md (complete)
- 🔄 CODE_STRUCTURE.md (draft)

### Week 40, Day 3 (Wednesday)
**Theme**: Code scaffolding & class design

```
9:00-10:00   Create header files (no implementation)
             - replication_manager.hpp
             - raft_extended.hpp
             - network_sender.hpp
             - batch_accumulator.hpp

10:00-11:00  Design class signatures
             - ReplicationManager::start()
             - ReplicationManager::push_event()
             - NetworkSender::send_batch()
             - BatchAccumulator::maybe_flush()

11:00-12:00  Create tests skeleton
             - replication_test.cpp structure
             - Test cases list (don't implement)
             - Mock objects outline

12:00-13:00  Update CODE_STRUCTURE.md
             - File locations
             - Dependencies between classes
             - Thread model diagram
```

**Deliverables**:
- ✅ 4 header files (signatures only)
- ✅ CODE_STRUCTURE.md (complete)
- ✅ Test skeleton (structure)

### Week 40, Day 4 (Thursday)
**Theme**: Network protocol finalization

```
9:00-10:00   Design network messages
             - HeartbeatRequest message
             - HeartbeatResponse message
             - BatchMessage format
             - AckMessage format

10:00-11:00  Create NETWORK_PROTOCOL.md
             - Message definitions (binary)
             - Serialization code (inline)
             - Example message dumps

11:00-12:00  Design recovery scenario
             - Follower startup
             - Follower sync (catch-up)
             - State consistency check

12:00-13:00  Create FAILOVER.md (complete)
             - Detection logic
             - Election timeline
             - Recovery process
```

**Deliverables**:
- ✅ NETWORK_PROTOCOL.md (complete)
- ✅ FAILOVER.md (complete)

### Week 40, Day 5 (Friday)
**Theme**: Documentation review & finalization

```
9:00-10:00   Review all design docs
             - DESIGN.md completeness
             - PROTOCOL.md clarity
             - CODE_STRUCTURE.md accuracy

10:00-11:00  Create PERFORMANCE_TARGETS.md
             - Throughput targets (60-70K)
             - Latency targets (p99 < 15ms)
             - CPU/Memory budgets
             - Benchmark methodology

11:00-12:00  Create TESTING_STRATEGY.md
             - Unit test plan
             - Integration test plan
             - Chaos test plan
             - Success criteria per test

12:00-13:00  Final review + polish
             - All docs complete
             - All diagrams consistent
             - INDEX.md updated

13:00-15:00  BUFFER: Catch up if behind
             - Reread design docs
             - Plan Week 41 in detail
             - Refine schedules
```

**Deliverables**:
- ✅ PERFORMANCE_TARGETS.md (complete)
- ✅ TESTING_STRATEGY.md (complete)
- ✅ distributed_layer/INDEX.md updated
- ✅ Week 40 Summary: 7 documentation files complete
- ✅ All code scaffolding in place (headers, no implementation)

### Week 40 Success Criteria
```
✅ DESIGN.md: 100% clear, no ambiguities
✅ PROTOCOL.md: Binary format fully specified
✅ CODE_STRUCTURE.md: Class design & interfaces
✅ FAILOVER.md: All scenarios covered
✅ PERFORMANCE_TARGETS.md: Metrics defined
✅ TESTING_STRATEGY.md: Test plan per component
✅ Header files: All signatures complete
✅ Test skeleton: All test cases identified
✅ No implementation code yet: Design only!
```

**Week 40 Effort**: ~40-45 hours  
**Code Lines**: 0 (design only)  
**Tests**: 0 (structure only)

---

## WEEK 41: RAFT & Replication Core

**Goal**: Core replication + RAFT consensus working on 3-node cluster  
**Output**: Working distributed system (no failover yet)  
**Time**: 50-60 hours  

### Week 41, Day 1 (Monday)
**Theme**: RAFT state machine completion

```
9:00-10:00   Extend existing RaftNode class
             - Add log replication methods
             - Add commit tracking
             - Add persistent log storage

10:00-11:00  Implement RaftNode::AppendLogEntry()
             - Persist to RocksDB
             - Update next_index for followers
             - Return commit point

11:00-12:00  Implement RaftNode::ApplyCommittedEntries()
             - Callback for distributed dedup
             - Check dedup consistency
             - Mark entries as applied

12:00-13:00  Unit tests: RAFT log management
             - Test append entry
             - Test log persistence
             - Test commit point tracking
             - Target: 8 tests passing
```

**Deliverables**:
- ✅ RaftNode extended (150 lines)
- ✅ 8 unit tests (100 lines)
- ✅ 0 network activity yet

### Week 41, Day 2 (Tuesday)
**Theme**: Batch accumulator & serialization

```
9:00-10:00   Implement BatchAccumulator class
             - Constructor (max_size=64, max_ms=1000)
             - push_event(event) → accumulate
             - try_flush() → pack batch if ready

10:00-11:00  Implement batch serialization
             - BatchMessage::serialize()
             - Pack header + events + footer
             - CRC32 checksum calculation

11:00-12:00  Implement batch deserialization
             - BatchMessage::deserialize()
             - Validate magic number
             - Verify checksum
             - Extract events

12:00-13:00  Unit tests: Batching & serialization
             - Test batch accumulation
             - Test serialization roundtrip
             - Test checksum verification
             - Test invalid batch detection
             - Target: 10 tests passing
```

**Deliverables**:
- ✅ BatchAccumulator (200 lines)
- ✅ Serialization (150 lines)
- ✅ 10 unit tests (150 lines)

### Week 41, Day 3 (Wednesday)
**Theme**: Replication queues & threading

```
9:00-10:00   Create ReplicationManager class
             - Constructor (takes core components)
             - start() → spawn threads
             - push_event_for_replication() → enqueue

10:00-11:00  Implement SPSC replication queues
             - Dispatcher → Replication queue
             - Network sender thread creation
             - Thread-safe initialization

11:00-12:00  Implement batch flushing mechanism
             - Timer-based flush (1ms)
             - Count-based flush (64 events)
             - Dequeue from SPSC

12:00-13:00  Unit tests: Threading & queuing
             - Test event enqueueing
             - Test thread safety
             - Test flush triggers
             - Target: 6 tests passing
```

**Deliverables**:
- ✅ ReplicationManager (250 lines)
- ✅ Threading logic (100 lines)
- ✅ 6 unit tests (100 lines)

### Week 41, Day 4 (Thursday)
**Theme**: Network I/O & follower communication

```
9:00-10:00   Implement NetworkSender class
             - Constructor (peer list)
             - connect_to_peers() → TCP sockets
             - send_batch() → async send

10:00-11:00  Implement batch sending
             - Serialize batch message
             - Send to Follower 1 (TCP)
             - Send to Follower 2 (TCP)
             - Non-blocking sends

11:00-12:00  Implement ack handling
             - Listen for follower acks
             - Update commit index when 2 acks
             - Timeout retry logic

12:00-13:00  Unit tests: Network communication
             - Test TCP connection setup
             - Test batch sending
             - Test ack reception
             - Target: 8 tests (with mock sockets)
```

**Deliverables**:
- ✅ NetworkSender (300 lines)
- ✅ Ack handling (100 lines)
- ✅ 8 unit tests (150 lines with mocks)

### Week 41, Day 5 (Friday)
**Theme**: Integration testing on 3-node cluster

```
9:00-11:00   Spin up 3-node test cluster
             - Start 3 instances on ports 9001,9002,9003
             - Configure leader/follower
             - Verify connectivity

11:00-12:00  Integration test: Event replication
             - Send 1000 events to leader
             - Verify all 3 nodes received them
             - Check dedup consistency
             - Check storage contents

12:00-13:00  Performance benchmark (preliminary)
             - Measure throughput (target: 60K+)
             - Measure latency p99 (target: < 20ms)
             - Measure CPU usage
             - Document baseline

13:00-15:00  Bug fixes & refinement
             - Fix any issues from integration test
             - Optimize batch sizes if needed
             - Plan Week 42 adjustments
```

**Deliverables**:
- ✅ 3-node cluster working
- ✅ 1000-event integration test passing
- ✅ Performance baseline documented
- ✅ Known issues list for Week 42

### Week 41 Success Criteria
```
✅ RAFT log replication working
✅ Batching (64/1ms) working
✅ Serialization roundtrip validated
✅ NetworkSender sends to 2 followers
✅ Ack handling updates commit index
✅ 3-node cluster spins up cleanly
✅ Events replicate to all nodes
✅ Dedup consistent across nodes
✅ Performance: 60K+ events/sec (preliminary)
✅ 48+ unit tests passing
✅ Integration test passing
```

**Week 41 Effort**: ~50-60 hours  
**Code Lines**: ~1500-1800 (implementation)  
**Tests**: 48+ unit tests + 1 integration test  
**Completion**: Core replication working, failover not yet

---

## WEEK 42: Failover, Testing, Polish

**Goal**: Complete distributed system with automatic failover  
**Output**: Production-ready (or close to it)  
**Time**: 50-60 hours  

### Week 42, Day 1 (Monday)
**Theme**: Leader election & detection

```
9:00-10:00   Implement heartbeat detection
             - RAFT heartbeat sender (leader only)
             - Heartbeat receiver (all nodes)
             - Timeout detection (3 missed beats)

10:00-11:00  Implement RAFT election logic
             - RequestVote RPC implementation
             - Voting logic (higher term/index)
             - Become leader transition

11:00-12:00  Implement follower state management
             - Follow current leader
             - Detect leader failure
             - Trigger election when needed

12:00-13:00  Unit tests: RAFT election
             - Test RequestVote handling
             - Test term advancement
             - Test leader election
             - Test split brain prevention
             - Target: 12 tests passing
```

**Deliverables**:
- ✅ Heartbeat mechanism (150 lines)
- ✅ Election logic (200 lines)
- ✅ 12 unit tests (180 lines)

### Week 42, Day 2 (Tuesday)
**Theme**: Failover scenarios & recovery

```
9:00-10:00   Implement state recovery
             - New leader reads RAFT log
             - Replays committed entries
             - Rebuilds dedup table consistency
             - Catches up with followers

10:00-11:00  Implement distributed dedup recovery
             - Sync dedup state across nodes
             - TTL-based cleanup coordination
             - Consistency checking

11:00-12:00  Implement graceful shutdown
             - Stop accepting new events
             - Wait for replication
             - Close connections
             - Exit cleanly

12:00-13:00  Unit tests: Failover & recovery
             - Test leader crash → election → recovery
             - Test follower crash → rejoin
             - Test dedup consistency after recovery
             - Target: 10 tests passing
```

**Deliverables**:
- ✅ State recovery (200 lines)
- ✅ Graceful shutdown (100 lines)
- ✅ 10 unit tests (150 lines)

### Week 42, Day 3 (Wednesday)
**Theme**: Comprehensive integration testing

```
9:00-10:00   Test: Normal replication
             - 3-node cluster steady state
             - 5000 events sent
             - All 3 nodes consistent
             - No duplicates

10:00-11:00  Test: Leader failure & recovery
             - Kill leader
             - New leader elected
             - Events continue being replicated
             - Old leader rejoins, catches up

11:00-12:00  Test: Network partition
             - Partition followers from leader
             - Old leader stops accepting events
             - Followers elect new leader
             - Partition heals, old leader rejoins

12:00-13:00  Test: Cascade failures
             - Kill 2 nodes (only 1 left)
             - Remaining node can't form quorum
             - Verify no decisions made
             - Nodes rejoin, recover
```

**Deliverables**:
- ✅ 4 comprehensive integration tests
- ✅ All tests passing
- ✅ Failure recovery validated

### Week 42, Day 4 (Thursday)
**Theme**: Performance testing & tuning

```
9:00-10:00   Run performance benchmark suite
             - Throughput at various loads
             - Latency percentiles (p50, p99, p999)
             - CPU utilization per node
             - Memory usage

10:00-11:00  Compare against targets
             - Target: 60-70K events/sec → Actual?
             - Target: p99 < 15ms → Actual?
             - Target: < 90% CPU → Actual?
             - Document differences

11:00-12:00  Optimize if needed
             - Adjust batch sizes
             - Tune thread priorities
             - Reduce allocations
             - Profile hotspots

12:00-13:00  Document performance results
             - Create PERFORMANCE_RESULTS.md
             - Compare vs single-node baseline
             - Identify bottlenecks
             - Recommendations for future
```

**Deliverables**:
- ✅ Comprehensive benchmark results
- ✅ PERFORMANCE_RESULTS.md
- ✅ Optimization notes

### Week 42, Day 5 (Friday)
**Theme**: Documentation & finalization

```
9:00-10:00   Complete all documentation
             - Update DESIGN.md with actual decisions
             - Complete CODE_STRUCTURE.md details
             - Finish DEPLOYMENT_GUIDE.md

10:00-11:00  Create operation playbooks
             - How to start cluster
             - How to monitor
             - How to debug issues
             - How to add/remove nodes

11:00-12:00  Final testing pass
             - Run all tests (100+ tests)
             - Verify performance targets met
             - Check documentation accuracy
             - Code review (self)

12:00-13:00  Wrap-up & summary
             - Update distributed_layer/INDEX.md
             - Create DAY42_COMPLETION_SUMMARY.md
             - Mark Week 42 complete
             - Plan next steps
```

**Deliverables**:
- ✅ Complete documentation
- ✅ Operation playbooks
- ✅ 100+ tests passing
- ✅ Performance targets met (or documented)
- ✅ DAY42_COMPLETION_SUMMARY.md

### Week 42 Success Criteria
```
✅ Leader election working
✅ Heartbeat detection working
✅ Automatic failover < 600ms
✅ State recovery working
✅ All 4 integration tests passing
✅ Normal replication test ✅
✅ Leader failure test ✅
✅ Network partition test ✅
✅ Cascade failure test ✅
✅ Performance: 60-70K events/sec (validated)
✅ Latency: p99 < 15ms (validated)
✅ CPU: < 90% (validated)
✅ 100+ unit + integration tests passing
✅ Complete documentation
✅ Ready for production (or early production)
```

**Week 42 Effort**: ~50-60 hours  
**Code Lines**: ~500-700 (failover, cleanup)  
**Tests**: 60+ unit tests + 4 integration tests  
**Completion**: Full distributed system with failover

---

## TOTAL EFFORT: 3 WEEKS

```
Week 40: Design only (no code)         = 40-50 hours
Week 41: Core replication              = 50-60 hours  
Week 42: Failover + comprehensive test = 50-60 hours

TOTAL: 140-170 hours (~3-4 weeks full-time)
```

## CODE OUTPUT

```
Week 40: 0 lines (design)
Week 41: 1500-1800 lines
Week 42: 500-700 lines
TOTAL: 2000-2500 lines of production C++ code
```

## TEST OUTPUT

```
Week 40: 0 tests (structure)
Week 41: 48+ unit tests + 1 integration
Week 42: 12+ unit tests + 4 integration tests
TOTAL: 100+ tests
```

## DAILY VALIDATION

After each day, you should have:
- ✅ Code compiling with zero warnings
- ✅ All new tests passing
- ✅ No regressions in core engine (38 tests still pass)
- ✅ Brief daily summary in NOTES/day_XX.md

---

## Contingency Plan

If you fall behind schedule:

### Option 1: Extend Week 42
- Move Day 5 testing to extra day
- Still finish by early Week 43
- Maintain quality

### Option 2: Defer Failover
- Complete Weeks 40-41 (basic replication)
- Defer failover logic to Week 43
- Still have working distributed system

### Option 3: Simplify Tests
- Defer some integration tests to Week 43
- Focus on unit tests first
- Add integration tests iteratively

### Option 4: Parallel Documentation
- Write docs while coding
- Reduce documentation effort
- Trade polish for speed

**Recommendation**: Option 1 (extend to 3.5 weeks if needed)

---

## Next Document

After completing Week 40:
1. Read this IMPLEMENTATION_PLAN.md (you are here)
2. Start Week 41 with PROTOCOL.md as reference
3. Use CODE_STRUCTURE.md to guide class organization
4. Follow daily checklist format

**Ready to start Week 40?** ✅

