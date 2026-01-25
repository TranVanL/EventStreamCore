## SEQUENCE DIAGRAMS - PLANTUML FORMAT
**Purpose**: Visual sequence flows for event processing | **Format**: PlantUML (renders on GitHub)

---

## DIAGRAM 1: Event Ingest & Dispatch

```plantuml
@startuml Event_Ingest_To_Dispatch
title Event Ingest & Dispatch (TCP → EventBus → Processors)
participant "TCP\nSocket" as tcp
participant "TCP\nParser" as parser
participant "Event\nPool" as pool
participant "EventBus\nDispatcher" as bus
participant "Processor\nQueues" as queues
participant "Metrics" as metrics

tcp -> parser: TCP frame arrives\n(256-512 bytes)
note right of tcp: t=0μs

parser -> parser: Validate magic\nDeserialize fields
note right of parser: t=2μs

parser -> pool: acquire()
note right of parser: Get from pre-alloc pool
pool -> parser: Event*
note right of pool: O(1), no malloc

parser -> bus: push(shared_ptr<Event>)
note right of parser: t=3μs\nLock-free SPSC

bus -> bus: pop() from inbound
note right of bus: t=5μs\nLock-free acquire

bus -> bus: Route by type\n(O(1) switch)

bus -> queues: push to\nprocessor queue
note right of bus: t=6μs\nLock-free SPSC

bus -> metrics: thread_local++\n(no lock!)
note right of metrics: t=6μs\nZero contention

note over tcp, metrics: Total Ingest: 6μs (p50) | 5-25ms (p99)

@enduml
```

---

## DIAGRAM 2: Realtime Processor Pipeline

```plantuml
@startuml Realtime_Processor
title Realtime Processor (Pass-Through with Dedup Check)
participant "Input\nQueue" as input
participant "Realtime\nProcessor" as processor
participant "Dedup\nTable" as dedup
participant "Storage\nQueue" as storage
participant "Metrics" as metrics
participant "Event\nPool" as pool

loop For Each Event
  input -> processor: pop()
  note right of input: Lock-free SPSC
  
  processor -> dedup: is_duplicate(id)
  note right of dedup: Lock-free read\nO(1) average
  
  alt Duplicate
    dedup -> processor: true
    processor -> storage: skip
    note right of processor: Drop duplicate
  else New Event
    dedup -> processor: false
    processor -> processor: Process event\n(business logic)
    note right of processor: 15-30μs
    
    processor -> storage: push(event)
    note right of storage: Lock-free SPSC
    
    processor -> metrics: local_count++
    note right of metrics: Thread-local\nNo lock!
  end
  
  storage -> pool: ~Event()\nreturn to pool
  note right of pool: Automatic RAII
end

note over input, pool: Throughput: 27K eps | Latency p99: <1ms

@enduml
```

---

## DIAGRAM 3: Transactional Processor (Idempotent)

```plantuml
@startuml Transactional_Processor
title Transactional Processor (Idempotent Processing)
participant "Input\nQueue" as input
participant "Processor" as processor
participant "Dedup\nInsert" as dedup
participant "Storage\nQueue" as storage
participant "Cleanup\nThread" as cleanup
participant "Metrics" as metrics

loop For Each Event
  input -> processor: pop()
  note right of input: Lock-free SPSC
  
  processor -> dedup: insert(id, timestamp)
  note right of dedup: Lock-free CAS\nNo lock on read!
  
  alt Already Exists
    dedup -> processor: false
    processor -> storage: skip
  else New Event (Insert Success)
    dedup -> processor: true
    processor -> processor: Process idempotent\n(exactly-once)
    note right of processor: 20-40μs
    
    processor -> storage: push(result)
    processor -> metrics: local_new++
  end
end

... (every 10 minutes) ...

cleanup -> dedup: cleanup()\nRemove old entries
note right of cleanup: Background thread\nNot in hot path

note over input, metrics: Throughput: 27K eps | Idempotent Window: 1 hour | Cleanup: Every 10min

@enduml
```

---

## DIAGRAM 4: Batch Processor (Windowed)

```plantuml
@startuml Batch_Processor
title Batch Processor (Windowed Aggregation)
participant "Input\nQueue" as input
participant "Batch\nProcessor" as processor
participant "Buckets\nMap" as buckets
participant "Storage\nQueue" as storage
participant "Metrics" as metrics

loop For Each Event
  input -> processor: pop()
  note right of input: Lock-free SPSC
  
  processor -> buckets: Get bucket for\n(topic, partition)
  note right of processor: O(1) hash lookup
  
  buckets -> processor: TopicBucket
  note right of buckets: Day 39: Contains\nevent vector +\nlast_flush_time
  
  processor -> buckets: Add to batch\nevents.push_back()
  
  processor -> processor: Check flush condition:\nSize >= 64 events OR\nTime >= 100ms
  
  alt Should Flush
    processor -> processor: Aggregate batch\n(100-500μs)
    buckets -> processor: Clear batch\nReset timer
    processor -> storage: push(aggregated\nresult)
    processor -> metrics: local_batches++
  else Keep Buffering
    processor -> metrics: (no action)
  end
end

note over input, metrics: Throughput: 27K eps | Batch Size: 64 events | Window: 100ms

@enduml
```

---

## DIAGRAM 5: Lock-Free Deduplication Details

```plantuml
@startuml Dedup_CAS_Insert
title Lock-Free Deduplicator (CAS-Based Insert)
participant "Thread A\n(Insert)" as a
participant "Bucket\nAtomic" as bucket
participant "Thread B\n(Lookup)" as b

box #FFE6E6
  participant "New Entry\nCAS Logic" as cas
end box

note over a, b: Both threads access same bucket simultaneously

par
  a -> cas: Calculate bucket index\nAllocate new Entry
  b -> bucket: load(acquire) - Get head
  note right of bucket: Lock-free read\nNo blocking!
end

a -> bucket: load(acquire) - Expected head
note right of a: Get current head value

par
  a -> cas: Check for duplicate\nin collision chain
  b -> bucket: Walk chain\nis_duplicate()
  note right of b: Lock-free linear search
end

alt Duplicate Found
  a -> cas: Found in chain
  cas -> a: Delete entry, return false
else New Entry
  a -> a: Link to current head\nPrepare for CAS
  
  a -> bucket: compare_exchange_strong()\nold_head → new_entry
  note right of bucket: Atomic CAS operation
  
  alt CAS Success
    bucket -> a: true
    note right of bucket: Entry inserted at head
    a -> a: Return true (inserted)
  else CAS Failed (Another thread won)
    bucket -> a: false
    a -> a: Retry (up to 3 times)\nor give up
  end
end

note over a, b: Read path: O(1) average, no lock\nInsert path: O(1) + CAS, 3 retry limit

@enduml
```

---

## DIAGRAM 6: Metrics Async Buffering (Day 39 Optimization)

```plantuml
@startuml Async_Metrics
title Metrics Async Buffering (1ms Thread-Local Buffer)
participant "Thread 1\n(Realtime)" as t1
participant "Thread 2\n(Transactional)" as t2
participant "Thread 3\n(Batch)" as t3
participant "Metrics\nRegistry" as registry
participant "Background\nReporter" as reporter

== Event Processing (Per Thread) ==

par
  t1 -> t1: Process event
  t1 -> t1: local_count++\n(NO LOCK!\nJust write)
  note right of t1: Thread-local\nZero contention

  t2 -> t2: Process event
  t2 -> t2: local_sum += latency\n(NO LOCK!)
  
  t3 -> t3: Process event
  t3 -> t3: local_count++\n(NO LOCK!)
end

note right of t1: 1000 events = 1ms\nat 1M event/sec

== Every 1000 Events (Or 1ms) ==

par
  t1 -> registry: Acquire lock\nFlush all 1000 events
  note right of registry: Only 1 lock!\nnot per event
  
  t2 -> t2: Continue processing\n(no wait for lock!)
  
  t3 -> t3: Continue processing\n(no blocking!)
end

registry -> registry: events_processed += 1000
registry -> registry: latency_sum aggregated
registry -> t1: Release lock

reporter -> registry: Query metrics every 100ms
registry -> reporter: Return snapshots
reporter -> reporter: Compute p50/p99/p99.9

note over t1, reporter: Lock reduction: 246K → 1K ops/sec (95%!)\nCPU improvement: 15% → 2% (87%!)

@enduml
```

---

## DIAGRAM 7: Event Memory Lifecycle (RAII)

```plantuml
@startuml Event_Lifecycle
title Event Memory Lifecycle (Reference Counting)
participant "Event\nPool" as pool
participant "TCP\nParser" as parser
participant "EventBus\nQueue" as bus
participant "Processor\nQueue" as proc_q
participant "Processor\nThread" as processor
participant "Storage\nQueue" as storage

pool -> pool: Startup: Create\n65536 events\navailable_count=65536
note right of pool: 32 MB pre-allocated

parser -> pool: acquire()
pool -> parser: Event* (ref_count=1)
note right of pool: available_count--\nEvent still in unique_ptr

parser -> bus: push(shared_ptr)
note right of bus: Create shared_ptr\nref_count=2
note left of bus: Pool still owns +\nQueue now references

parser -> parser: Return
note right of parser: Parser no longer\nneeds event

bus -> proc_q: Route & push\n(copy shared_ptr)
note right of proc_q: ref_count=3\nPool + Queue1 + Queue2

bus -> parser: (next event)

proc_q -> processor: pop() + process
note right of processor: ref_count still=3

processor -> storage: push(processed)
note right of storage: Copy shared_ptr again\nref_count=4

processor -> processor: ~shared_ptr()\nDestuctor called
note right of processor: ref_count--=3

proc_q -> proc_q: overwrite queue slot
note right of proc_q: Old event's\nshared_ptr destroyed\nref_count--=2

storage -> storage: Write complete\n~shared_ptr()
note right of storage: ref_count--=1

note over pool: ref_count=1 (only pool's unique_ptr owns it)
pool -> pool: release()\navailable_count++
note right of pool: Event back in\navailable list

note over parser, pool: Total E2E: ~50μs (p50) to ~5ms (p99)\nGuarantees: No use-after-free, No leaks, RAII cleanup

@enduml
```

---

## DIAGRAM 8: Complete System Flow (High Level)

```plantuml
@startuml Complete_Flow
title Complete EventStreamCore Flow
participant "Network\n(TCP)" as net
participant "Ingest\nThread" as ingest
participant "EventBus\nThread" as bus
participant "Realtime\nThread" as real
participant "Transactional\nThread" as trans
participant "Batch\nThread" as batch
participant "Storage\nThread" as storage
participant "Metrics\nThread" as metrics

net -> ingest: TCP frame
activate ingest
ingest -> ingest: Parse (2μs)\nPool acquire (0.1μs)
ingest -> bus: push(event)\nLock-free SPSC
deactivate ingest

activate bus
bus -> bus: Route by type\n(O(1) switch)
bus -> real: Push if REALTIME
bus -> trans: Push if TRANSACTIONAL
bus -> batch: Push if BATCH
deactivate bus

par Parallel Processing
  activate real
  real -> real: Pop + Process (30μs)
  real -> storage: Push result
  deactivate real
  
  activate trans
  trans -> trans: Dedup check (lock-free)\nProcess (40μs)
  trans -> storage: Push result
  deactivate trans
  
  activate batch
  batch -> batch: Batch aggregation\n(100-500μs)\nFlush if needed
  batch -> storage: Push aggregated
  deactivate batch
end

activate storage
storage -> storage: Write to backend\n(async)
deactivate storage

activate metrics
metrics -> metrics: Thread-local update\nno lock!
metrics -> metrics: Flush every 1ms\n(1 lock per 1000)
deactivate metrics

note over net, metrics: Single event journey: 50μs-5ms\n82K events/sec sustained throughput

@enduml
```

---

## DIAGRAM 9: SPSC Ring Buffer (Lock-Free)

```plantuml
@startuml SPSC_RingBuffer
title SPSC Ring Buffer (Producer ↔ Consumer)
participant "Producer\nThread" as prod
participant "Ring\nBuffer" as buffer
participant "Consumer\nThread" as cons

buffer -> buffer: Init: head_=0, tail_=0\ncapacity=16384

par Concurrent Operations
  prod -> buffer: PUSH: Load head_\n(memory_order_relaxed)
  cons -> buffer: POP: Load tail_\n(memory_order_relaxed)
end

prod -> buffer: Check if full:\nLoad tail_\n(memory_order_acquire)
note right of prod: Synchronize with consumer

cons -> buffer: Check if empty:\nLoad head_\n(memory_order_acquire)
note right of cons: Synchronize with producer

alt Buffer Not Full
  prod -> buffer: Write data to\nbuffer[head_]
  prod -> buffer: Store new head_\n(memory_order_release)
  note right of prod: Publish update
else Buffer Full
  prod -> prod: Return false (overflow)
end

alt Buffer Not Empty
  cons -> buffer: Read data from\nbuffer[tail_]
  cons -> buffer: Store new tail_\n(memory_order_release)
  note right of cons: Publish update
else Buffer Empty
  cons -> cons: Return empty
end

note over prod, cons: No CAS loops!\nMonotonic indices prevent ABA\nDay 39: alignas(64) prevents false sharing

@enduml
```

---

## How to Use These Diagrams

### View on GitHub
1. PlantUML diagrams render automatically in GitHub Markdown
2. Click on the diagram to expand
3. Each sequence shows the flow with timing annotations

### Rendering Locally
```bash
# Install plantuml
sudo apt-get install plantuml

# Generate PNG from markdown
plantuml -Tpng diagram_name.md -o output.png
```

### Key Takeaways per Diagram

| Diagram | Key Insight |
|---------|------------|
| 1 | Zero-copy ingest, lock-free dispatch, async metrics |
| 2 | Realtime fast path, immediate processing |
| 3 | Transactional lock-free dedup, idempotency guarantee |
| 4 | Batch windowing, consolidated maps (Day 39 opt) |
| 5 | Lock-free CAS, collision chaining, no blocking reads |
| 6 | Thread-local buffering, 95% contention reduction |
| 7 | RAII reference counting, automatic cleanup |
| 8 | Complete system picture, all components working together |
| 9 | Memory ordering without CAS, false sharing fix |

---

