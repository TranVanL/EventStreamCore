"""
FastAPI adapter — exposes EventStreamCore as a REST + Prometheus service.

Run:
    ESCCORE_LIB=/path/to/libesccore.so esccore-adapter
    # or: uvicorn esccore.adapter:app --host 0.0.0.0 --port 8000
"""

from __future__ import annotations

import os
from contextlib import asynccontextmanager
from typing import Optional

import uvicorn
from fastapi import FastAPI, HTTPException
from fastapi.responses import PlainTextResponse
from pydantic import BaseModel

from esccore.engine import Engine, EscError
from esccore.types import Priority

# ── Global engine (initialised at startup) ────────────────────────────────────

_engine: Optional[Engine] = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _engine
    lib = os.environ.get("ESCCORE_LIB", "libesccore.so")
    config = os.environ.get("ESCCORE_CONFIG", "config/config.yaml")
    _engine = Engine(lib)
    _engine.init(config)
    yield
    _engine.shutdown()
    _engine = None


app = FastAPI(
    title="EventStreamCore Adapter",
    description="REST gateway to the EventStreamCore engine",
    version="1.0.0",
    lifespan=lifespan,
)


# ── Models ────────────────────────────────────────────────────────────────────

class EventIn(BaseModel):
    id: int = 0
    topic: str
    body: bytes = b""
    priority: str = "MEDIUM"


class BatchIn(BaseModel):
    events: list[EventIn]


# ── Routes ────────────────────────────────────────────────────────────────────

@app.post("/events", status_code=202)
def push_event(event: EventIn):
    """Push a single event into the core engine."""
    try:
        pri = Priority[event.priority.upper()]
        _engine.push(event.topic, event.body, pri, event.id)
        return {"status": "accepted"}
    except EscError as e:
        raise HTTPException(status_code=503, detail=str(e))


@app.post("/events/batch", status_code=202)
def push_batch(batch: BatchIn):
    """Push a batch of events."""
    items = [
        (e.topic, e.body, Priority[e.priority.upper()], e.id)
        for e in batch.events
    ]
    pushed = _engine.push_batch(items)
    return {"pushed": pushed, "total": len(items)}


@app.get("/metrics", response_class=PlainTextResponse)
def prometheus_metrics():
    """Prometheus text exposition endpoint."""
    return _engine.metrics_prometheus()


@app.get("/health")
def health_check():
    """Liveness + readiness probe."""
    h = _engine.health()
    status_code = 200 if h.is_ready else 503
    return {
        "alive": h.is_alive,
        "ready": h.is_ready,
        "backpressure_level": h.backpressure_level,
    }


@app.post("/pipeline/pause", status_code=200)
def pause_pipeline():
    _engine.pause()
    return {"status": "paused"}


@app.post("/pipeline/resume", status_code=200)
def resume_pipeline():
    _engine.resume()
    return {"status": "resumed"}


# ── CLI entry point ───────────────────────────────────────────────────────────

def main():
    host = os.environ.get("ESCCORE_HOST", "0.0.0.0")
    port = int(os.environ.get("ESCCORE_PORT", "8000"))
    uvicorn.run("esccore.adapter:app", host=host, port=port, log_level="info")


if __name__ == "__main__":
    main()
