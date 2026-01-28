# Distributed System Architecture

**EventStreamCore Distributed Migration Plan** — Scaling from single-node to multi-node cluster with C++ core + Python gRPC + service mesh.

---

## 🎯 Vision

Transform EventStreamCore from a **high-performance single-node engine** into a **fault-tolerant distributed system** capable of processing billions of events/day across multiple data centers.

### Goals

- **Horizontal Scalability**: Add nodes to increase throughput linearly
- **Fault Tolerance**: Survive node failures without data loss
- **Geographic Distribution**: Process events close to data sources
- **Elastic Scaling**: Auto-scale based on load
- **Multi-Protocol**: HTTP REST, gRPC, native binary protocol

---

## 🏗️ Architecture Overview

```
┌──────────────────────────────────────────────────────────────────┐
│                    External Clients                               │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐                 │
│  │ HTTP/REST  │  │   gRPC     │  │   Native   │                 │
│  │  Clients   │  │  Clients   │  │ TCP/UDP    │                 │
│  └──────┬─────┘  └──────┬─────┘  └──────┬─────┘                 │
└─────────┼────────────────┼────────────────┼───────────────────────┘
          │                │                │
┌─────────▼────────────────▼────────────────▼───────────────────────┐
│                   Python API Gateway Layer                        │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │              FastAPI / gRPC Server                        │    │
│  │  - HTTP REST endpoints (POST /events, GET /metrics)      │    │
│  │  - gRPC streaming (bidirectional event streams)          │    │
│  │  - Authentication & authorization (JWT, OAuth2)          │    │
│  │  - Rate limiting & quota management                      │    │
│  └────────────────────────┬─────────────────────────────────┘    │
└───────────────────────────┼──────────────────────────────────────┘
                            │
                   ┌────────▼────────┐
                   │  Load Balancer  │
                   │  (Consul/Envoy) │
                   └────────┬────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
┌───────▼────────┐  ┌───────▼────────┐  ┌───────▼────────┐
│ C++ Core Node 1│  │ C++ Core Node 2│  │ C++ Core Node N│
│ (EventStream)  │  │ (EventStream)  │  │ (EventStream)  │
│                │  │                │  │                │
│ ┌────────────┐ │  │ ┌────────────┐ │  │ ┌────────────┐ │
│ │MPSC Queue  │ │  │ │MPSC Queue  │ │  │ │MPSC Queue  │ │
│ │Dispatcher  │ │  │ │Dispatcher  │ │  │ │Dispatcher  │ │
│ │Workers     │ │  │ │Workers     │ │  │ │Workers     │ │
│ └────────────┘ │  │ └────────────┘ │  │ └────────────┘ │
│                │  │                │  │                │
│ ┌────────────┐ │  │ ┌────────────┐ │  │ ┌────────────┐ │
│ │Raft Leader │ │  │ │Raft Follow.│ │  │ │Raft Follow.│ │
│ │(Consensus) │ │  │ │(Replica)   │ │  │ │(Replica)   │ │
│ └────────────┘ │  │ └────────────┘ │  │ └────────────┘ │
└────────┬───────┘  └────────┬───────┘  └────────┬───────┘
         │                   │                   │
         └───────────────────┼───────────────────┘
                             │
                   ┌─────────▼──────────┐
                   │  Persistent Storage │
                   │  (RocksDB / Redis)  │
                   └─────────────────────┘
```

---

## 📦 Components

### 1. C++ Core Nodes (EventStreamCore)

**Role**: High-performance event processing engine

**Responsibilities**:
- TCP/UDP binary protocol ingestion (existing)
- Lock-free MPSC/SPSC queue processing (existing)
- NUMA-aware worker threading (existing)
- **NEW**: Inter-node communication (gRPC)
- **NEW**: Raft consensus for state replication
- **NEW**: Event sharding & routing

**Technologies**:
- **Language**: C++20
- **RPC**: gRPC (C++ bindings)
- **Consensus**: NuRaft library
- **Metrics**: Prometheus C++ client

**Code Changes Required**:
```cpp
// New components to add:
include/distributed/
  ├── NodeDiscovery.hpp     // Consul integration
  ├── RaftEngine.hpp        // Consensus layer
  ├── ShardManager.hpp      // Event partitioning
  └── InterNodeRPC.hpp      // gRPC service definitions

src/distributed/
  ├── NodeDiscovery.cpp
  ├── RaftEngine.cpp
  ├── ShardManager.cpp
  └── InterNodeRPC.cpp
```

---

### 2. Python API Gateway

**Role**: External client interface & protocol translation

**Responsibilities**:
- HTTP REST API (FastAPI)
- gRPC streaming endpoints
- Authentication & authorization (JWT, OAuth2)
- Rate limiting (Redis-based)
- Protocol conversion (HTTP/gRPC → Binary TCP)
- Metrics aggregation

**Technologies**:
- **Framework**: FastAPI (async Python 3.11+)
- **gRPC**: grpcio + grpcio-tools
- **Auth**: python-jose (JWT), authlib (OAuth2)
- **Caching**: redis-py
- **Monitoring**: prometheus-client

**Project Structure**:
```
api_gateway/
├── app/
│   ├── main.py              # FastAPI application
│   ├── routers/
│   │   ├── events.py        # POST /events, GET /events/{id}
│   │   ├── metrics.py       # GET /metrics
│   │   └── admin.py         # Admin endpoints
│   ├── grpc/
│   │   ├── event_service.proto
│   │   └── event_service_pb2_grpc.py
│   ├── auth/
│   │   ├── jwt_handler.py
│   │   └── oauth2.py
│   └── clients/
│       └── cpp_core_client.py  # gRPC client to C++ nodes
├── tests/
├── requirements.txt
└── Dockerfile
```

**API Endpoints**:

| Method | Path | Description |
|--------|------|-------------|
| POST | `/v1/events` | Submit event (JSON body) |
| GET | `/v1/events/{id}` | Query event status |
| GET | `/v1/metrics` | System metrics (Prometheus) |
| POST | `/v1/topics/subscribe` | Subscribe to topic |
| WS | `/v1/stream` | WebSocket event stream |

**gRPC Services**:
```protobuf
service EventService {
  rpc SubmitEvent(EventRequest) returns (EventResponse);
  rpc StreamEvents(stream EventRequest) returns (stream EventResponse);
  rpc QueryMetrics(MetricsRequest) returns (MetricsResponse);
}
```

---

### 3. Service Discovery (Consul)

**Role**: Node registration & health checking

**Features**:
- Automatic node discovery
- Health checks (HTTP /health endpoint)
- Key-value store for configuration
- Service mesh integration (Consul Connect)

**Configuration**:
```json
{
  "service": {
    "name": "eventstream-core",
    "port": 8081,
    "check": {
      "http": "http://localhost:8081/health",
      "interval": "10s",
      "timeout": "2s"
    }
  }
}
```

---

### 4. Load Balancer (Envoy)

**Role**: Intelligent request routing & circuit breaking

**Features**:
- Round-robin load balancing
- Health-based routing (exclude unhealthy nodes)
- Circuit breaker (fail fast on overload)
- Retry logic with exponential backoff
- TLS termination

**Envoy Config** (simplified):
```yaml
clusters:
- name: eventstream_cluster
  connect_timeout: 0.25s
  type: STRICT_DNS
  lb_policy: ROUND_ROBIN
  load_assignment:
    cluster_name: eventstream_cluster
    endpoints:
    - lb_endpoints:
      - endpoint:
          address:
            socket_address:
              address: eventstream-node-1
              port_value: 8081
      - endpoint:
          address:
            socket_address:
              address: eventstream-node-2
              port_value: 8081
```

---

### 5. Consensus Layer (Raft)

**Role**: State replication & leader election

**Use Cases**:
- Topic table synchronization across nodes
- Configuration changes (add/remove nodes)
- Event offset tracking (exactly-once semantics)

**Implementation** (NuRaft library):
```cpp
class RaftEngine {
public:
    void initializeRaft(int nodeId, const std::vector<std::string>& peers);
    bool isLeader() const;
    void replicateTopicTable(const TopicTable& table);
    TopicTable getReplicatedTopicTable();
    
private:
    nuraft::ptr<nuraft::raft_server> raftServer_;
    nuraft::ptr<StateMachine> stateMachine_;
};
```

**Raft State Machine**:
- **Logs**: Topic subscribe/unsubscribe operations
- **Snapshots**: Full TopicTable state (periodic)
- **Replication**: Majority quorum (N/2 + 1)

---

## 🔄 Migration Strategy

### Phase 2.1: Single C++ Node + Python Gateway (2 months)

**Goal**: Decouple external API from core engine

**Steps**:
1. Create Python FastAPI project
2. Implement gRPC client to existing C++ node
3. Add HTTP REST endpoints (POST /events)
4. Deploy side-by-side (Python gateway → C++ core)
5. Benchmark latency overhead (~200-300µs expected)

**Deliverables**:
- ✅ Python API gateway Docker image
- ✅ gRPC protocol definitions (event_service.proto)
- ✅ Integration tests (HTTP → gRPC → C++)
- ✅ Performance baseline (throughput, latency)

---

### Phase 2.2: Multi-Node C++ Cluster (3 months)

**Goal**: Horizontal scalability with manual sharding

**Steps**:
1. Implement Consul service discovery in C++
2. Add gRPC server to C++ nodes (inter-node RPC)
3. Implement ShardManager (consistent hashing by topic)
4. Deploy 3-node cluster with Envoy load balancer
5. Test failover scenarios (kill node, verify no data loss)

**Deliverables**:
- ✅ C++ gRPC server implementation
- ✅ Sharding logic (topic → node mapping)
- ✅ Envoy configuration
- ✅ Chaos testing (node failures, network partitions)

---

### Phase 2.3: Raft Consensus Integration (2 months)

**Goal**: Automatic failover & state replication

**Steps**:
1. Integrate NuRaft library into C++ codebase
2. Implement Raft state machine (TopicTable replication)
3. Add leader election & follower redirection
4. Test split-brain scenarios (network partition)
5. Benchmark consensus overhead (~1ms latency)

**Deliverables**:
- ✅ Raft-replicated TopicTable
- ✅ Leader election tests
- ✅ Snapshot & log compaction
- ✅ Multi-DC replication (optional)

---

## 📊 Performance Targets

| Metric | Single-Node | 3-Node Cluster | 10-Node Cluster |
|--------|-------------|----------------|-----------------|
| Throughput | 8.5M/s | 25M/s | 80M/s |
| P99 Latency | 3.8 µs | 5.2 µs | 8.1 µs |
| Availability | 99.9% | 99.99% | 99.999% |
| Recovery Time | N/A | < 10s | < 5s |

**Latency Breakdown** (3-node cluster):

| Component | Latency |
|-----------|---------|
| Python gateway (HTTP → gRPC) | 250 µs |
| Envoy routing | 180 µs |
| Network (1Gbps LAN) | 120 µs |
| C++ processing (existing) | 3.8 µs |
| Raft replication (async) | 1.2 ms |
| **Total (sync path)** | **554 µs** |

---

## 🔐 Security

### Authentication

- **JWT Tokens**: Signed with RS256 (public/private key)
- **OAuth2**: Integration with GitHub, Google, Okta
- **API Keys**: Long-lived tokens for service accounts

### Authorization

- **RBAC**: Role-based access control (admin, user, readonly)
- **Topic-Level ACLs**: Per-topic publish/subscribe permissions
- **Rate Limiting**: Per-user quotas (Redis-based)

### Transport Security

- **TLS 1.3**: All external connections (client → gateway)
- **mTLS**: Inter-node communication (C++ ↔ C++)
- **Certificate Rotation**: Automatic renewal (Let's Encrypt)

---

## 🧪 Testing Strategy

### Unit Tests

- Python API gateway: pytest (80% coverage)
- C++ core: Google Test (existing)

### Integration Tests

- End-to-end: HTTP → Python → gRPC → C++
- Multi-node: 3-node cluster deployment
- Failover: Kill leader, verify follower promotion

### Performance Tests

- Throughput: Apache Bench (ab), wrk
- Latency: Custom C++ benchmark (histogram)
- Stress: Simulate 100K concurrent connections

### Chaos Engineering

- Random node kills (kill -9)
- Network partitions (iptables)
- Slow disk (fio + throttling)

---

## 🚀 Deployment

### Docker Compose (Development)

```yaml
version: '3.8'
services:
  consul:
    image: consul:1.17
    ports: ["8500:8500"]
  
  eventstream-node-1:
    build: ./EventStreamCore
    environment:
      - NODE_ID=1
      - CONSUL_ADDR=consul:8500
    depends_on: [consul]
  
  eventstream-node-2:
    build: ./EventStreamCore
    environment:
      - NODE_ID=2
      - CONSUL_ADDR=consul:8500
    depends_on: [consul]
  
  api-gateway:
    build: ./api_gateway
    ports: ["8080:8080"]
    environment:
      - CORE_NODES=eventstream-node-1:8081,eventstream-node-2:8081
```

### Kubernetes (Production)

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: eventstream-core
spec:
  serviceName: eventstream
  replicas: 3
  selector:
    matchLabels:
      app: eventstream-core
  template:
    metadata:
      labels:
        app: eventstream-core
    spec:
      containers:
      - name: core
        image: eventstream:v2.0
        ports:
        - containerPort: 8081
        env:
        - name: NODE_ID
          valueFrom:
            fieldRef:
              fieldPath: metadata.name
        resources:
          requests:
            cpu: 4
            memory: 8Gi
          limits:
            cpu: 8
            memory: 16Gi
```

---

## 📚 Next Steps

- **[Microservices Integration](../microservices/)**: Go REST API, Rust authentication
- **[Core Engine](../core/)**: Current single-node architecture

---

*Last updated: January 2026*
