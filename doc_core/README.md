# 📦 Core Engine Documentation

> Complete technical documentation for EventStreamCore internals.

---

## 📚 Contents

| Document | Description |
|----------|-------------|
| [🏗️ architecture.md](architecture.md) | System design, data flow |
| [🔗 queues.md](queues.md) | Lock-free SPSC/MPSC |
| [📨 event.md](event.md) | Event model, protocol |
| [💾 memory.md](memory.md) | NUMA memory pools |

---

## 🎯 Core Pillars

```
┌─────────────────────────────────────────────────────────────┐
│                    EVENTSTREAM CORE                          │
├───────────────┬───────────────┬───────────────┬─────────────┤
│  Lock-Free    │  Zero-Alloc   │     NUMA      │   Binary    │
│   Queues      │   Pools       │  Optimization │  Protocol   │
├───────────────┼───────────────┼───────────────┼─────────────┤
│ • SPSC ~8ns   │ • Pre-alloc   │ • CPU pinning │ • Length    │
│ • MPSC ~20ns  │ • Thread-local│ • Local RAM   │   prefixed  │
│ • No locks    │ • No malloc   │ • 40% faster  │ • Zero-copy │
└───────────────┴───────────────┴───────────────┴─────────────┘
```

---

## 🚀 Performance

| Component | Latency | Throughput |
|-----------|---------|------------|
| SpscRingBuffer | **8ns** | 125M ops/s |
| MpscQueue | **20ns** | 52M ops/s |
| EventPool | **11ns** | 89M ops/s |
| **End-to-End** | **< 2µs P99** | **10M+ events/s** |

---

## 📖 Reading Order

1. **[architecture.md](architecture.md)** - Big picture
2. **[event.md](event.md)** - Event & protocol
3. **[queues.md](queues.md)** - Lock-free deep dive
4. **[memory.md](memory.md)** - NUMA details
