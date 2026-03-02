# EventStreamCore — Go SDK

Wraps `libesccore.so` via **cgo** and ships ready-made adapters:

- **gRPC service** — high-performance binary protocol for Go/Java/Rust clients
- **HTTP health probe** — Kubernetes liveness/readiness
- **Prometheus exporter** — pull-based metrics

## Quick start

```go
package main

import (
    "log"
    esc "github.com/eventstreamcore/sdk-go/esccore"
)

func main() {
    engine, err := esc.New("../../build/libesccore.so")
    if err != nil { log.Fatal(err) }
    defer engine.Shutdown()

    engine.Init("config/config.yaml")

    engine.Push(esc.Event{
        Topic:    "sensor/temperature",
        Body:     []byte{0x42},
        Priority: esc.PriorityHIGH,
    })

    m, _ := engine.Metrics()
    log.Printf("Processed: %d", m.TotalEventsProcessed)
}
```

## gRPC adapter

```bash
cd sdk/go
go run ./cmd/grpc-adapter \
    -lib ../../build/libesccore.so \
    -config ../../config/config.yaml \
    -port 50051
```
