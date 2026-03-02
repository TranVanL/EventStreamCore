# EventStreamCore — Python SDK

Thin Python wrapper around `libesccore.so` using **ctypes**.
No compiled extensions — just point it at the shared library and go.

## Install

```bash
pip install -e sdk/python
```

## Quick start

```python
from esccore import Engine

engine = Engine("/path/to/libesccore.so")
engine.init("config/config.yaml")

engine.push(topic="sensor/temperature", body=b"\x42", priority="HIGH")

metrics = engine.metrics()
print(f"Processed: {metrics.total_events_processed}")

engine.shutdown()
```

## FastAPI adapter

```bash
# Starts REST + Prometheus /metrics on port 8000
ESCCORE_LIB=/path/to/libesccore.so esccore-adapter
```

| Endpoint | Description |
|----------|-------------|
| `POST /events` | Push an event |
| `POST /events/batch` | Push a batch of events |
| `GET /metrics` | Prometheus text exposition |
| `GET /health` | Health check |
| `POST /pipeline/pause` | Pause processing |
| `POST /pipeline/resume` | Resume processing |
