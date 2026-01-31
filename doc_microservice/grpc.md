# 🔗 gRPC Gateway

> Multi-language client integration via gRPC.

---

## 🎯 Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                      gRPC GATEWAY                                    │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                     Clients                                  │    │
│  │                                                              │    │
│  │   ┌─────────┐    ┌─────────┐    ┌─────────┐                 │    │
│  │   │ Python  │    │   Go    │    │ Node.js │                 │    │
│  │   └────┬────┘    └────┬────┘    └────┬────┘                 │    │
│  │        │              │              │                       │    │
│  │        │   gRPC (HTTP/2, Protobuf)   │                      │    │
│  │        │              │              │                       │    │
│  └────────┼──────────────┼──────────────┼───────────────────────┘    │
│           │              │              │                            │
│           └──────────────┴──────────────┘                            │
│                          │                                           │
│                          ▼                                           │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                   gRPC Server                                 │   │
│  │                   Port: 9200                                  │   │
│  │                                                               │   │
│  │   ┌───────────────────────────────────────────────────────┐  │   │
│  │   │  EventService                                          │  │   │
│  │   │  ├─ Publish(Event) → Ack                              │  │   │
│  │   │  ├─ PublishBatch(Events) → Ack                        │  │   │
│  │   │  ├─ Subscribe(Topic) → stream Event                   │  │   │
│  │   │  └─ HealthCheck() → Status                            │  │   │
│  │   └───────────────────────────────────────────────────────┘  │   │
│  │                                                               │   │
│  └───────────────────────────┬───────────────────────────────────┘   │
│                              │                                       │
│                              ▼                                       │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                 EventStreamCore Engine                        │   │
│  │                 (Native TCP/UDP path)                         │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 📋 Proto Definition

```protobuf
syntax = "proto3";

package eventstream;

// Main service
service EventService {
  // Publish single event
  rpc Publish(PublishRequest) returns (PublishResponse);
  
  // Publish batch of events
  rpc PublishBatch(PublishBatchRequest) returns (PublishResponse);
  
  // Subscribe to topic (server streaming)
  rpc Subscribe(SubscribeRequest) returns (stream Event);
  
  // Bidirectional streaming
  rpc Stream(stream Event) returns (stream Event);
  
  // Health check
  rpc HealthCheck(Empty) returns (HealthResponse);
}

// Event priority
enum Priority {
  BATCH = 0;
  LOW = 1;
  MEDIUM = 2;
  HIGH = 3;
  CRITICAL = 4;
}

// Event message
message Event {
  string id = 1;
  string topic = 2;
  bytes payload = 3;
  Priority priority = 4;
  int64 timestamp_ns = 5;
  map<string, string> metadata = 6;
}

// Publish request
message PublishRequest {
  Event event = 1;
}

// Batch publish
message PublishBatchRequest {
  repeated Event events = 1;
}

// Publish response
message PublishResponse {
  bool success = 1;
  string message = 2;
  int64 latency_ns = 3;
}

// Subscribe request
message SubscribeRequest {
  string topic_pattern = 1;  // e.g., "sensors/*"
  Priority min_priority = 2;
}

// Health response
message HealthResponse {
  bool healthy = 1;
  string version = 2;
  int64 uptime_seconds = 3;
}

message Empty {}
```

---

## 🐍 Python Client

### Installation

```bash
pip install grpcio grpcio-tools

# Generate Python code from proto
python -m grpc_tools.protoc \
  -I. \
  --python_out=. \
  --grpc_python_out=. \
  eventstream.proto
```

### Publish Events

```python
import grpc
from eventstream_pb2 import Event, PublishRequest, Priority
from eventstream_pb2_grpc import EventServiceStub

# Connect
channel = grpc.insecure_channel('localhost:9200')
client = EventServiceStub(channel)

# Create event
event = Event(
    id="evt-12345",
    topic="sensors/temperature",
    payload=b'{"value": 23.5, "unit": "celsius"}',
    priority=Priority.HIGH,
    metadata={"sensor_id": "temp-001", "location": "room-1"}
)

# Publish
response = client.Publish(PublishRequest(event=event))
print(f"Published: {response.success}, latency: {response.latency_ns}ns")
```

### Subscribe to Events

```python
from eventstream_pb2 import SubscribeRequest, Priority

# Subscribe to topic pattern
request = SubscribeRequest(
    topic_pattern="sensors/*",
    min_priority=Priority.LOW
)

# Stream events
for event in client.Subscribe(request):
    print(f"Received: {event.topic}")
    print(f"  Payload: {event.payload.decode()}")
    print(f"  Priority: {Priority.Name(event.priority)}")
```

### Batch Publish

```python
from eventstream_pb2 import PublishBatchRequest

events = [
    Event(topic="metrics/cpu", payload=b'{"value": 45}', priority=Priority.LOW),
    Event(topic="metrics/mem", payload=b'{"value": 8192}', priority=Priority.LOW),
    Event(topic="metrics/disk", payload=b'{"value": 75}', priority=Priority.LOW),
]

response = client.PublishBatch(PublishBatchRequest(events=events))
print(f"Batch published: {response.success}")
```

---

## 🔵 Go Client

```go
package main

import (
    "context"
    "log"
    "time"
    
    "google.golang.org/grpc"
    pb "eventstream/proto"
)

func main() {
    // Connect
    conn, err := grpc.Dial("localhost:9200", grpc.WithInsecure())
    if err != nil {
        log.Fatal(err)
    }
    defer conn.Close()
    
    client := pb.NewEventServiceClient(conn)
    
    // Publish
    ctx, cancel := context.WithTimeout(context.Background(), time.Second)
    defer cancel()
    
    event := &pb.Event{
        Id:       "evt-12345",
        Topic:    "orders/new",
        Payload:  []byte(`{"order_id": "ORD-001", "amount": 99.99}`),
        Priority: pb.Priority_HIGH,
    }
    
    resp, err := client.Publish(ctx, &pb.PublishRequest{Event: event})
    if err != nil {
        log.Fatal(err)
    }
    
    log.Printf("Published: %v, latency: %dns", resp.Success, resp.LatencyNs)
}
```

---

## 📊 Performance

```
┌─────────────────────────────────────────────────────────────────────┐
│                    gRPC vs Native TCP                                │
│                                                                      │
│  Benchmark: 1M events, localhost                                    │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐ │
│  │                                                                 │ │
│  │   Native TCP:                                                   │ │
│  │   ├── Latency P99:  1.8µs                                      │ │
│  │   └── Throughput:   10.2M events/sec                           │ │
│  │                                                                 │ │
│  │   gRPC (unary):                                                 │ │
│  │   ├── Latency P99:  45µs                                       │ │
│  │   └── Throughput:   850K events/sec                            │ │
│  │                                                                 │ │
│  │   gRPC (streaming):                                             │ │
│  │   ├── Latency P99:  12µs                                       │ │
│  │   └── Throughput:   2.1M events/sec                            │ │
│  │                                                                 │ │
│  │   gRPC (batch 100):                                             │ │
│  │   ├── Latency P99:  180µs (per batch)                          │ │
│  │   └── Throughput:   5.5M events/sec                            │ │
│  │                                                                 │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  Recommendation:                                                     │
│  • Ultra-low latency: Use native TCP                                │
│  • Ease of use: Use gRPC streaming                                  │
│  • High throughput: Use gRPC batch                                  │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 🔒 Security

```yaml
# TLS configuration
grpc:
  port: 9200
  tls:
    enabled: true
    cert_file: /etc/eventstream/server.crt
    key_file: /etc/eventstream/server.key
    ca_file: /etc/eventstream/ca.crt
    client_auth: require  # mTLS
```

```python
# Python client with TLS
credentials = grpc.ssl_channel_credentials(
    root_certificates=open('ca.crt', 'rb').read(),
    private_key=open('client.key', 'rb').read(),
    certificate_chain=open('client.crt', 'rb').read()
)

channel = grpc.secure_channel('eventstream.example.com:9200', credentials)
```

---

## ➡️ Next

- [Kubernetes Deployment →](kubernetes.md)
