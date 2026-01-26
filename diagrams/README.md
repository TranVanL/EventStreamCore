# UML Sequence Diagrams - EventStreamCore

Individual PlantUML diagram files for the EventStreamCore project. These can be previewed using VS Code extensions or rendered locally.

## Diagrams

1. **01_Event_Ingest_To_Dispatch.puml** - Event ingestion from TCP socket through dispatch to processor queues
2. **02_Realtime_Processor.puml** - Realtime processor with pass-through and dedup logic
3. **03_Transactional_Processor.puml** - Transactional processor with idempotent processing guarantees
4. **04_Batch_Processor.puml** - Batch processor with windowed aggregation
5. **05_Dedup_CAS_Insert.puml** - Lock-free deduplicator using Compare-and-Swap (CAS)
6. **06_Async_Metrics.puml** - Metrics async buffering with thread-local optimization
7. **07_Event_Lifecycle.puml** - Event memory lifecycle with reference counting
8. **08_Complete_Flow.puml** - Complete system flow showing all components together
9. **09_SPSC_RingBuffer.puml** - SPSC ring buffer lock-free queue implementation

## Viewing the Diagrams

### Option 1: VS Code PlantUML Extension
Install the PlantUML extension:
```
extension id: jebbs.plantuml
```

Then open any `.puml` file and use the preview (Ctrl+Shift+P → PlantUML: Preview)

### Option 2: Online Viewer
- Use [PlantUML Online Editor](http://www.plantuml.com/plantuml/uml/)
- Copy-paste the content of any `.puml` file

### Option 3: Generate PNG/SVG Locally
```bash
# Install PlantUML (requires Java)
# Windows: choco install plantuml
# Linux: sudo apt-get install plantuml
# Mac: brew install plantuml

# Generate from this directory
plantuml *.puml
```

## Key Insights

| Diagram | Focus | Key Performance Metric |
|---------|-------|----------------------|
| 01 | Zero-copy ingest, lock-free dispatch | 6μs (p50) |
| 02 | Realtime fast path, immediate processing | 27K eps |
| 03 | Lock-free dedup, idempotency guarantee | 1-hour window |
| 04 | Batch windowing, consolidated maps | 64 events / 100ms |
| 05 | CAS-based insert without blocking reads | O(1) average |
| 06 | Thread-local buffering, contention reduction | 95% lock reduction |
| 07 | RAII reference counting, automatic cleanup | No use-after-free |
| 08 | Complete system architecture | 82K eps sustained |
| 09 | Memory ordering without CAS loops | False sharing fix |

## Architecture Summary

- **Ingest**: TCP parser → Lock-free SPSC queue
- **Dispatch**: O(1) topic-based routing → Type-specific processor queues
- **Processing**: 
  - Realtime: Fast path, dedup check
  - Transactional: Exactly-once semantics
  - Batch: Windowed aggregation
- **Storage**: Async queue for persistence
- **Metrics**: Thread-local buffering with periodic flush

## Performance Targets

- **Single Event Latency**: 50μs (p50) → 5ms (p99)
- **Throughput**: 82K events/sec sustained
- **CPU Usage**: 2% for metrics (down from 15%)
- **Memory**: 32MB pre-allocated event pool (65536 events)

---

For detailed technical documentation, see [CORE_ENGINE_TECHNICAL_DOCUMENTATION.md](../engine_documents/CORE_ENGINE_TECHNICAL_DOCUMENTATION.md)
