# Week 40 Day 1 - START HERE

**Date**: Monday, January 27, 2026 (or whenever you start)  
**Goal**: Understand distributed architecture completely before writing any code  
**Time Available**: 8-10 hours today  
**Output**: Clear notes on architecture, ready to write PROTOCOL.md tomorrow

---

## Your Schedule Today

### 9:00-10:00 (1 hour) - Set up environment
```
[ ] Review all existing DESIGN.md and IMPLEMENTATION_PLAN.md
[ ] Understand 3-node topology
[ ] Understand batching strategy (64 events / 1ms)
[ ] Note down any questions
```

### 10:00-11:00 (1 hour) - Deep dive: Replication
```
[ ] How leader sends events to followers
[ ] How followers ack back
[ ] How commit index gets updated
[ ] When an event is considered "committed"
[ ] Draw this on whiteboard or paper

Key insight: 
  Event sent to leader
    ↓
  Batched with 63 others (or 1ms timeout)
    ↓
  Serialized and sent to both followers
    ↓
  Followers ack: "Got batch #5"
    ↓
  Leader gets 2 acks (quorum)
    ↓
  Event is now "committed" (safe)
```

### 11:00-12:00 (1 hour) - Deep dive: RAFT
```
[ ] How leader sends heartbeats (150ms interval)
[ ] How followers detect leader death (3 missed beats = 450ms)
[ ] How election happens (< 150ms)
[ ] What happens to buffered events during election
[ ] Why 3 nodes matter (quorum = 2 of 3)

Key insight:
  Normal: Leader healthy
    ↓ (450ms passes, no events coming in)
  Detection: Followers see no heartbeat
    ↓
  Election: Follower 1 asks "vote for me?"
    ↓
  Winner: Gets 2 votes (self + other)
    ↓
  Recovery: New leader starts accepting events
```

### 12:00-13:00 (1 hour) - Break/Lunch

### 13:00-14:00 (1 hour) - Serialization deep dive
```
[ ] What data needs to be sent between nodes?
[ ] How to pack events efficiently?
[ ] Checksum calculation (CRC32)
[ ] Network framing (magic number, end marker)
[ ] Error recovery (corrupt batch detection)

Key insight:
  BATCH_MESSAGE = [HEADER] [EVENTS] [FOOTER]
  
  HEADER (48 bytes):
    - magic = 0xDEADBEEF (validation)
    - batch_id = auto-increment (ordering)
    - event_count = 1-64 (how many events)
    - timestamp = when batched (for debugging)
    - checksum = CRC32 of payload (integrity)
  
  EVENTS (up to 32KB):
    - packed event data
    - already serialized from core
  
  FOOTER (8 bytes):
    - end_marker = 0x01DEADBEEF (sync)
```

### 14:00-15:00 (1 hour) - Threading model
```
[ ] How many new threads needed?
[ ] What does replication thread do?
[ ] What does batch accumulator do?
[ ] What does network sender do?
[ ] How do they synchronize (SPSC queues)?

Key insight:
  Dispatcher (existing)
    ↓ (push event)
  Replication SPSC queue (lock-free, NEW)
    ↓ (pop from queue)
  Batch accumulator thread (NEW)
    - Wait for 64 events or 1ms
    - Serialize batch
    - Push to network queue
    ↓ (pop from queue)
  Network sender thread (NEW)
    - Send to Follower 1 (async)
    - Send to Follower 2 (async)
    - Wait for ack
    - Update commit index
```

### 15:00-16:00 (1 hour) - Edge cases
```
[ ] What if follower is slow?
[ ] What if leader crashes mid-replication?
[ ] What if network partition happens?
[ ] What if follower recovers after crash?
[ ] What about duplicate detection?

Key edge cases:
  1. Slow follower:
     Leader doesn't block on replication
     Continues accepting events
     Slow follower catches up asynchronously
  
  2. Leader crash:
     Events not replicated = lost (acceptable)
     Events replicated but not acked = may reappear
     Dedup prevents actual duplicates
  
  3. Network partition:
     Old leader isolated = can't commit (no quorum)
     Followers elect new leader = continues
     Partition heals = old leader rejoins as follower
  
  4. Follower recovery:
     Reads RAFT log from storage
     Replays committed batches
     Dedup prevents re-processing
  
  5. Dedup consistency:
     Each node has own dedup table
     Dedup state replicates with events
     Eventual consistency (not strong)
```

### 16:00-17:00 (1 hour) - Whiteboard & notes
```
[ ] Draw complete flow on whiteboard:
    - Normal replication (happy path)
    - Leader election
    - Follower rejoin
    
[ ] Write down key timing numbers:
    - Heartbeat interval: 150ms
    - Detection timeout: 450ms (3 × 150ms)
    - Election time: 150ms
    - Total failover: ~600ms
    
[ ] Write down constraints:
    - Batch size: max 64 events
    - Batch timeout: max 1ms
    - Network: TCP sockets
    - Storage: RocksDB for RAFT log
    
[ ] List questions for tomorrow:
    - Any ambiguities in protocol?
    - Any implementation concerns?
    - Any performance worries?
```

### 17:00-18:00 (1 hour) - Buffer/Catch-up
```
[ ] Review any sections still unclear
[ ] Reread relevant DESIGN.md sections
[ ] Prepare questions for Day 2
[ ] Mental model should be complete by now
```

---

## Your Deliverable Today

### By End of Day
You should have:
1. ✅ Complete understanding of 3-node architecture
2. ✅ Whiteboard drawings of:
   - Normal replication flow
   - Leader election
   - Failover scenario
3. ✅ Key numbers written down (150ms, 450ms, 600ms, 64 events, 1ms)
4. ✅ List of questions (if any)
5. ✅ Confidence: "I understand how this system works"

### Key Insight You Should Have
```
"The system has TWO channels:

1. DATA CHANNEL (Event Replication):
   - Async batching (fast, efficient)
   - Leader → Followers
   - No blocking
   - Committed when 2 acks received

2. CONSENSUS CHANNEL (RAFT):
   - Leader election
   - State consistency
   - Safety guarantees
   - Not on hot path (heartbeat every 150ms)

These two channels don't interfere.
Data channel is fast.
Consensus channel is robust."
```

---

## Tomorrow (Day 2)

When you're done with today, tomorrow you'll write PROTOCOL.md:
- Binary message format
- Serialization examples
- Validation rules
- Network frame structure

You'll feel confident because you'll understand it deeply now.

---

## If You Get Stuck

### Stuck on 3-node topology?
→ Reread DESIGN.md section "System Overview"
→ Draw it on paper
→ Answer: "What's the role of each node?"

### Stuck on replication flow?
→ Reread DESIGN.md section "Event Batching Strategy"
→ Draw it as pipeline
→ Answer: "When is event safely replicated?"

### Stuck on RAFT?
→ Reread DESIGN.md section "RAFT Consensus Layer"
→ Focus on "Leader Election (Simplified)"
→ Answer: "What detects leader death? How long?"

### Stuck on threading?
→ Reread DESIGN.md section "Threading Model"
→ Draw threads + queues
→ Answer: "Which thread does what?"

---

## Success Metrics for Today

```
By 6 PM, you should be able to:

✅ Explain 3-node topology in < 2 minutes
✅ Draw event replication flow from memory
✅ Explain RAFT leader election in < 3 minutes
✅ List key timing numbers (150ms, 450ms, 600ms)
✅ Explain edge case: "Leader crash during replication"
✅ Explain edge case: "Network partition"
✅ Know why batching is 64/1ms (not 1/0ms or 1000/10ms)
✅ Know why 3 nodes matter (quorum = majority)
✅ Know when event is "committed"
✅ Know how dedup prevents duplicates across cluster
```

---

## Night Before Tips

- Don't code anything today (design only!)
- Have paper/whiteboard ready
- Read DESIGN.md in one sitting (you need full context)
- Take breaks every 1 hour (distributed systems is brain-heavy)
- Make notes, not just reading
- Questions are OK - write them down for Day 2

---

## Ready to Start?

When you're ready:
1. [ ] Open distributed_layer/DESIGN.md
2. [ ] Get whiteboard + markers
3. [ ] Get paper + pen
4. [ ] Follow the 9-hour schedule above
5. [ ] At 6 PM, you'll have complete mental model
6. [ ] Tomorrow: Write PROTOCOL.md with confidence

**Estimated time**: 8-10 hours (spread across full day, with breaks)  
**Output**: Complete architectural understanding  
**Next step**: Write PROTOCOL.md (binary spec)  

---

Let me know when you start! 🚀

