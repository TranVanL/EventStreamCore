# Microservices Integration

**Go & Rust Auxiliary Services** — REST API, monitoring dashboards, authentication, and observability layer.

---

## 🎯 Purpose

While the **C++ core handles ultra-low latency event processing** and **Python provides the gRPC gateway**, we need additional services for:

- **External API**: Human-friendly REST API (Go)
- **Monitoring**: Real-time dashboards & alerting (Go)
- **Authentication**: High-security auth service (Rust)
- **Observability**: Distributed tracing & log aggregation (Go)

---

## 🏗️ Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                        Clients                                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐      │
│  │ Web UI   │  │ Mobile   │  │ CLI Tool │  │ External │      │
│  │ (React)  │  │   App    │  │  (curl)  │  │   API    │      │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘      │
└───────┼─────────────┼─────────────┼─────────────┼────────────┘
        │             │             │             │
        └─────────────┼─────────────┼─────────────┘
                      │             │
                ┌─────▼─────────────▼──────┐
                │   Go REST API Gateway    │
                │   (Gin/Fiber framework)  │
                │   - /v1/events           │
                │   - /v1/topics           │
                │   - /v1/stats            │
                └─────────┬────────────────┘
                          │
              ┌───────────┼───────────┐
              │           │           │
    ┌─────────▼──┐  ┌─────▼─────┐  ┌▼────────────┐
    │ Rust Auth  │  │ Python    │  │ C++ Core    │
    │  Service   │  │  Gateway  │  │ (Direct)    │
    │ (Actix)    │  │  (gRPC)   │  │ (Binary)    │
    └────────────┘  └───────────┘  └─────────────┘
          │
    ┌─────▼──────┐
    │  Postgres  │
    │ (Users/ACL)│
    └────────────┘

┌────────────────────────────────────────────────────────────────┐
│                    Observability Layer                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐         │
│  │  Prometheus  │  │    Jaeger    │  │     Loki     │         │
│  │  (Metrics)   │  │  (Tracing)   │  │    (Logs)    │         │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘         │
│         │                  │                  │                 │
│         └──────────────────┼──────────────────┘                 │
│                            │                                    │
│                   ┌────────▼────────┐                          │
│                   │  Grafana        │                          │
│                   │  (Dashboards)   │                          │
│                   └─────────────────┘                          │
└────────────────────────────────────────────────────────────────┘
```

---

## 📦 Services

### 1. Go REST API Gateway

**Purpose**: Human-friendly HTTP REST API for web/mobile clients

**Why Go?**
- Excellent HTTP performance (Gin, Fiber frameworks)
- Easy JSON marshaling
- Strong standard library
- Better for CRUD operations than C++

**Endpoints**:

| Method | Path | Description | Auth |
|--------|------|-------------|------|
| POST | `/v1/events` | Submit event (JSON) | JWT |
| GET | `/v1/events/{id}` | Query event by ID | JWT |
| GET | `/v1/topics` | List all topics | JWT |
| POST | `/v1/topics/{name}/subscribe` | Subscribe to topic | JWT |
| GET | `/v1/stats/realtime` | Real-time metrics | API Key |
| GET | `/v1/health` | Health check | Public |

**Example Request**:
```bash
curl -X POST http://api.eventstream.io/v1/events \
  -H "Authorization: Bearer <JWT_TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{
    "topic": "sensor/temperature",
    "payload": {
      "value": 23.5,
      "unit": "celsius",
      "timestamp": "2026-01-28T10:30:00Z"
    },
    "priority": 5
  }'
```

**Response**:
```json
{
  "event_id": "evt_1a2b3c4d",
  "status": "accepted",
  "timestamp": "2026-01-28T10:30:00.123Z",
  "latency_ms": 2.4
}
```

**Project Structure**:
```
rest_api_gateway/
├── cmd/
│   └── server/
│       └── main.go
├── internal/
│   ├── handlers/
│   │   ├── events.go
│   │   ├── topics.go
│   │   └── stats.go
│   ├── middleware/
│   │   ├── auth.go
│   │   └── ratelimit.go
│   ├── clients/
│   │   ├── python_grpc_client.go  # Call Python gateway
│   │   └── cpp_tcp_client.go      # Direct C++ binary protocol
│   └── models/
│       └── event.go
├── pkg/
│   ├── jwt/
│   └── metrics/
├── configs/
│   └── config.yaml
├── go.mod
└── Dockerfile
```

**Code Example** (handlers/events.go):
```go
package handlers

import (
    "net/http"
    "github.com/gin-gonic/gin"
)

type EventHandler struct {
    cppClient  *clients.CPPClient
    authClient *clients.AuthClient
}

func (h *EventHandler) CreateEvent(c *gin.Context) {
    var req EventRequest
    if err := c.ShouldBindJSON(&req); err != nil {
        c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
        return
    }
    
    // Validate JWT (call Rust auth service)
    user, err := h.authClient.ValidateToken(c.GetHeader("Authorization"))
    if err != nil {
        c.JSON(http.StatusUnauthorized, gin.H{"error": "invalid token"})
        return
    }
    
    // Check ACL (can user publish to this topic?)
    if !h.authClient.CanPublish(user.ID, req.Topic) {
        c.JSON(http.StatusForbidden, gin.H{"error": "no permission"})
        return
    }
    
    // Forward to C++ core (binary protocol over TCP)
    eventID, err := h.cppClient.SubmitEvent(req.Topic, req.Payload, req.Priority)
    if err != nil {
        c.JSON(http.StatusInternalServerError, gin.H{"error": "submission failed"})
        return
    }
    
    c.JSON(http.StatusCreated, EventResponse{
        EventID: eventID,
        Status:  "accepted",
    })
}
```

---

### 2. Rust Authentication Service

**Purpose**: High-security authentication & authorization

**Why Rust?**
- Memory safety (critical for auth service)
- Zero-cost abstractions
- Strong type system (prevent auth bugs)
- Excellent crypto libraries (ring, rustls)

**Responsibilities**:
- JWT token generation & validation
- OAuth2 integration (GitHub, Google)
- User management (CRUD)
- Topic-level ACL (who can publish/subscribe)
- Audit logging (security events)

**API Endpoints**:

| Method | Path | Description |
|--------|------|-------------|
| POST | `/auth/login` | Username/password login |
| POST | `/auth/token/validate` | Validate JWT token |
| POST | `/auth/token/refresh` | Refresh expired token |
| GET | `/auth/users/{id}/permissions` | Get user ACLs |
| POST | `/auth/acl/grant` | Grant topic permission |

**Project Structure**:
```
auth_service/
├── src/
│   ├── main.rs
│   ├── handlers/
│   │   ├── auth.rs
│   │   ├── users.rs
│   │   └── acl.rs
│   ├── models/
│   │   ├── user.rs
│   │   └── permission.rs
│   ├── crypto/
│   │   ├── jwt.rs
│   │   └── password.rs
│   ├── db/
│   │   └── postgres.rs
│   └── middleware/
│       └── logging.rs
├── migrations/
│   └── 001_initial_schema.sql
├── Cargo.toml
└── Dockerfile
```

**Code Example** (crypto/jwt.rs):
```rust
use jsonwebtoken::{encode, decode, Header, Validation, EncodingKey, DecodingKey};
use serde::{Serialize, Deserialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct Claims {
    pub sub: String,  // User ID
    pub exp: usize,   // Expiration timestamp
    pub roles: Vec<String>,
}

pub fn generate_token(user_id: &str, roles: Vec<String>) -> Result<String, JWTError> {
    let claims = Claims {
        sub: user_id.to_string(),
        exp: (chrono::Utc::now() + chrono::Duration::hours(24)).timestamp() as usize,
        roles,
    };
    
    let secret = std::env::var("JWT_SECRET")?;
    let token = encode(
        &Header::default(),
        &claims,
        &EncodingKey::from_secret(secret.as_bytes())
    )?;
    
    Ok(token)
}

pub fn validate_token(token: &str) -> Result<Claims, JWTError> {
    let secret = std::env::var("JWT_SECRET")?;
    let token_data = decode::<Claims>(
        token,
        &DecodingKey::from_secret(secret.as_bytes()),
        &Validation::default()
    )?;
    
    Ok(token_data.claims)
}
```

**Database Schema** (Postgres):
```sql
CREATE TABLE users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    username VARCHAR(255) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    email VARCHAR(255),
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE topic_acl (
    id SERIAL PRIMARY KEY,
    user_id UUID REFERENCES users(id),
    topic_pattern VARCHAR(255) NOT NULL,  -- e.g., "sensor/*"
    permission VARCHAR(50) NOT NULL,      -- "publish" or "subscribe"
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE INDEX idx_acl_user ON topic_acl(user_id);
CREATE INDEX idx_acl_topic ON topic_acl(topic_pattern);
```

---

### 3. Go Monitoring Dashboard

**Purpose**: Real-time system metrics & alerting

**Why Go?**
- Prometheus client library (official)
- Easy to integrate with Grafana
- Good concurrency for real-time data

**Features**:
- **Metrics Collection**: Scrape from C++ nodes (Prometheus exporter)
- **Dashboards**: Grafana integration (pre-built dashboards)
- **Alerting**: Slack/PagerDuty notifications
- **Custom Metrics**: Business KPIs (events/topic, latency by priority)

**Metrics Exposed**:

| Metric | Type | Description |
|--------|------|-------------|
| `eventstream_events_total` | Counter | Total events processed |
| `eventstream_latency_seconds` | Histogram | End-to-end latency |
| `eventstream_queue_depth` | Gauge | Current MPSC queue size |
| `eventstream_worker_utilization` | Gauge | Worker thread CPU % |
| `eventstream_errors_total` | Counter | Errors by type |

**Prometheus Config**:
```yaml
scrape_configs:
  - job_name: 'eventstream-core'
    static_configs:
      - targets: ['node1:8081', 'node2:8081', 'node3:8081']
    scrape_interval: 5s
```

**Grafana Dashboard JSON** (excerpt):
```json
{
  "dashboard": {
    "title": "EventStreamCore Metrics",
    "panels": [
      {
        "title": "Throughput (events/sec)",
        "targets": [
          {
            "expr": "rate(eventstream_events_total[1m])",
            "legendFormat": "{{node}}"
          }
        ]
      },
      {
        "title": "P99 Latency",
        "targets": [
          {
            "expr": "histogram_quantile(0.99, eventstream_latency_seconds_bucket)",
            "legendFormat": "P99"
          }
        ]
      }
    ]
  }
}
```

---

### 4. Go Distributed Tracing (Jaeger)

**Purpose**: Trace requests across services

**Flow**:
```
HTTP Request → Go API → Rust Auth → Python Gateway → C++ Core
   (span1)      (span2)   (span3)      (span4)        (span5)
```

**OpenTelemetry Integration** (Go):
```go
import (
    "go.opentelemetry.io/otel"
    "go.opentelemetry.io/otel/trace"
)

func (h *EventHandler) CreateEvent(c *gin.Context) {
    tracer := otel.Tracer("rest-api")
    ctx, span := tracer.Start(c.Request.Context(), "create_event")
    defer span.End()
    
    // Call Rust auth service (propagate trace context)
    user, err := h.authClient.ValidateTokenWithContext(ctx, token)
    
    // Call C++ core (inject trace ID in binary frame header)
    eventID, err := h.cppClient.SubmitEventWithContext(ctx, req)
    
    span.SetAttributes(attribute.String("event_id", eventID))
}
```

**Jaeger UI**:
- View end-to-end latency breakdown
- Identify bottlenecks (e.g., "Rust auth takes 80% of latency")
- Track error propagation across services

---

## 🚀 Deployment

### Docker Compose (All Services)

```yaml
version: '3.8'

services:
  # Go REST API
  rest-api:
    build: ./rest_api_gateway
    ports: ["8080:8080"]
    environment:
      - AUTH_SERVICE_URL=http://auth-service:8090
      - CPP_CORE_NODES=cpp-node-1:8081,cpp-node-2:8081
    depends_on: [auth-service]
  
  # Rust Auth Service
  auth-service:
    build: ./auth_service
    ports: ["8090:8090"]
    environment:
      - DATABASE_URL=postgres://user:pass@postgres:5432/auth
      - JWT_SECRET=supersecret
    depends_on: [postgres]
  
  # Postgres (Auth DB)
  postgres:
    image: postgres:15
    environment:
      POSTGRES_DB: auth
      POSTGRES_USER: user
      POSTGRES_PASSWORD: pass
    volumes:
      - postgres-data:/var/lib/postgresql/data
  
  # Prometheus
  prometheus:
    image: prom/prometheus:v2.40.0
    ports: ["9090:9090"]
    volumes:
      - ./monitoring/prometheus.yml:/etc/prometheus/prometheus.yml
  
  # Grafana
  grafana:
    image: grafana/grafana:9.3.0
    ports: ["3000:3000"]
    environment:
      - GF_SECURITY_ADMIN_PASSWORD=admin
    volumes:
      - ./monitoring/dashboards:/etc/grafana/provisioning/dashboards
  
  # Jaeger
  jaeger:
    image: jaegertracing/all-in-one:1.40
    ports: ["16686:16686", "14268:14268"]

volumes:
  postgres-data:
```

---

## 📊 Performance Expectations

| Service | Latency (P99) | Throughput | Language |
|---------|---------------|------------|----------|
| Go REST API | 15 ms | 50K req/s | Go |
| Rust Auth | 3 ms | 100K req/s | Rust |
| Python Gateway | 250 µs | 200K req/s | Python |
| C++ Core | 3.8 µs | 8.5M events/s | C++ |

**Total Latency** (HTTP → C++):
```
15 ms (Go) + 3 ms (Rust) + 0.25 ms (Python) + 0.0038 ms (C++) ≈ 18.3 ms
```

---

## 🔐 Security Best Practices

1. **Secrets Management**: Use HashiCorp Vault or AWS Secrets Manager
2. **Rate Limiting**: Redis-based token bucket (100 req/min per user)
3. **Input Validation**: Strict JSON schema validation (Go validator)
4. **SQL Injection**: Use parameterized queries (sqlx in Rust)
5. **CORS**: Restrict origins in production (`cors` middleware)
6. **TLS**: Enforce HTTPS in production (Let's Encrypt)

---

## 📚 References

- **Go API Framework**: [Gin](https://gin-gonic.com/), [Fiber](https://gofiber.io/)
- **Rust Auth**: [Actix-web](https://actix.rs/), [jsonwebtoken](https://docs.rs/jsonwebtoken/)
- **Monitoring**: [Prometheus](https://prometheus.io/), [Grafana](https://grafana.com/)
- **Tracing**: [OpenTelemetry](https://opentelemetry.io/), [Jaeger](https://www.jaegertracing.io/)

---

*Last updated: January 2026*
