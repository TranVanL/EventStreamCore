"""
FastAPI adapter — exposes EventStreamCore observability as a REST service
and forwards processed events to downstream microservices via callbacks.

Run:
    ESCCORE_LIB=/path/to/libesccore.so esccore-adapter
    # or: uvicorn esccore.adapter:app --host 0.0.0.0 --port 8000
"""

from __future__ import annotations

import os
from contextlib import asynccontextmanager
from typing import Optional

import uvicorn
from fastapi import FastAPI
from fastapi.responses import JSONResponse

from esccore.engine import Engine, EscError
from esccore.types import _EscEvent

import ctypes

# ── Global engine ─────────────────────────────────────────────────────────────

_engine: Optional[Engine] = None


def _on_event(event_ptr, _user_data) -> int:
    """Default callback: log each processed event to stdout."""
    evt = event_ptr.contents
    topic = evt.topic.decode(errors="replace") if evt.topic else ""
    body  = bytes(evt.body[:evt.body_len]) if evt.body and evt.body_len else b""
    print(f"[event] id={evt.id} topic={topic!r} body={body!r}")
    return 0  # keep receiving


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _engine
    lib    = os.environ.get("ESCCORE_LIB", "libesccore.so")
    config = os.environ.get("ESCCORE_CONFIG", "config/config.yaml")
    prefix = os.environ.get("ESCCORE_TOPIC_PREFIX", "")

    _engine = Engine(lib)
    _engine.init(config)
    _engine.subscribe(topic_prefix=prefix, callback=_on_event)
    yield
    _engine.shutdown()
    _engine = None


app = FastAPI(
    title="EventStreamCore Adapter",
    description="Observability gateway — events flow out of the engine via callbacks",
    version="2.0.0",
    lifespan=lifespan,
)


# ── Observability routes ──────────────────────────────────────────────────────

@app.get("/metrics")
def get_metrics():
    """Return current engine metrics."""
    m = _engine.metrics()
    return {
        "total_events_processed": m.total_events_processed,
        "total_events_dropped":   m.total_events_dropped,
        "queue_depth":            m.queue_depth,
        "backpressure_level":     m.backpressure_level,
    }


@app.get("/health")
def health_check():
    """Liveness + readiness probe."""
    h = _engine.health()
    status_code = 200 if h.is_ready else 503
    return JSONResponse(
        status_code=status_code,
        content={
            "alive":             h.is_alive,
            "ready":             h.is_ready,
            "backpressure_level": h.backpressure_level,
        },
    )


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
