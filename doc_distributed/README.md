# 🌐 Distributed Documentation

> Cluster setup, Raft consensus, and replication.

---

## 📚 Contents

| Document | Description |
|----------|-------------|
| [🗳️ raft.md](raft.md) | Raft consensus algorithm |
| [🖥️ cluster.md](cluster.md) | Cluster configuration |
| [📋 replication.md](replication.md) | Log replication details |

---

## 🎯 Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                    DISTRIBUTED ARCHITECTURE                          │
│                                                                      │
│                        ┌─────────────┐                              │
│                        │   Leader    │                              │
│                        │   Node 1    │                              │
│                        └──────┬──────┘                              │
│                               │                                      │
│              ┌────────────────┼────────────────┐                    │
│              │                │                │                    │
│              ▼                ▼                ▼                    │
│       ┌───────────┐    ┌───────────┐    ┌───────────┐              │
│       │ Follower  │    │ Follower  │    │ Follower  │              │
│       │  Node 2   │    │  Node 3   │    │  Node N   │              │
│       └───────────┘    └───────────┘    └───────────┘              │
│                                                                      │
│   • Strong consistency (linearizable)                                │
│   • Automatic leader election                                        │
│   • Survives N/2 - 1 failures                                       │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 🔑 Key Features

| Feature | Description |
|---------|-------------|
| **Raft Consensus** | Leader-based, strongly consistent |
| **Auto Failover** | Election within 150-300ms |
| **Log Replication** | All writes replicated to majority |
| **Dedup Sync** | Idempotency state across cluster |

---

## 📖 Reading Order

1. **[raft.md](raft.md)** - Understand consensus
2. **[cluster.md](cluster.md)** - Setup nodes
3. **[replication.md](replication.md)** - Replication details
