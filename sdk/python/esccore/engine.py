"""Core engine wrapper — loads libesccore.so and exposes a Pythonic API."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
from typing import Optional, Sequence

from esccore.types import (
    Priority, Metrics, Health,
    _EscEvent, _EscMetrics, _EscHealth,
)


class EscError(RuntimeError):
    """Raised when a C API call returns a non-zero status."""

    _MESSAGES = {
        -1: "Engine not initialised or already shut down",
        -2: "Backpressure — try again later",
        -3: "Invalid argument",
        -4: "Operation timed out",
        -5: "Queue is full",
        -99: "Internal engine error",
    }

    def __init__(self, code: int) -> None:
        self.code = code
        super().__init__(self._MESSAGES.get(code, f"Unknown error ({code})"))


def _check(rc: int) -> None:
    if rc != 0:
        raise EscError(rc)


class Engine:
    """
    High-level Python interface to the EventStreamCore engine.

    Usage::

        engine = Engine("/path/to/libesccore.so")
        engine.init("config/config.yaml")
        engine.push("sensor/temp", b"\\x42", Priority.HIGH)
        print(engine.metrics())
        engine.shutdown()
    """

    def __init__(self, lib_path: Optional[str] = None) -> None:
        path = lib_path or os.environ.get("ESCCORE_LIB", "libesccore.so")
        self._lib = ctypes.CDLL(path)
        self._bind_functions()
        self._initialised = False

    # ── lifecycle ─────────────────────────────────────────────────────────

    def init(self, config_path: str = "config/config.yaml") -> None:
        _check(self._lib.esccore_init(config_path.encode()))
        self._initialised = True

    def shutdown(self) -> None:
        _check(self._lib.esccore_shutdown())
        self._initialised = False

    # ── publish ───────────────────────────────────────────────────────────

    def push(
        self,
        topic: str,
        body: bytes = b"",
        priority: Priority = Priority.MEDIUM,
        event_id: int = 0,
    ) -> None:
        evt = self._make_event(event_id, priority, topic, body)
        _check(self._lib.esccore_push(ctypes.byref(evt)))

    def push_batch(
        self,
        events: Sequence[tuple[str, bytes, Priority, int]],
    ) -> int:
        """Push many events.  Returns how many were accepted."""
        arr_type = _EscEvent * len(events)
        arr = arr_type()
        for i, (topic, body, pri, eid) in enumerate(events):
            arr[i] = self._make_event(eid, pri, topic, body)

        pushed = ctypes.c_size_t(0)
        self._lib.esccore_push_batch(arr, len(events), ctypes.byref(pushed))
        return pushed.value

    # ── observability ─────────────────────────────────────────────────────

    def metrics(self) -> Metrics:
        m = _EscMetrics()
        _check(self._lib.esccore_metrics(ctypes.byref(m)))
        return Metrics._from_c(m)

    def health(self) -> Health:
        h = _EscHealth()
        _check(self._lib.esccore_health(ctypes.byref(h)))
        return Health._from_c(h)

    def metrics_prometheus(self) -> str:
        buf = ctypes.create_string_buffer(4096)
        written = ctypes.c_size_t(0)
        _check(self._lib.esccore_metrics_prometheus(buf, 4096, ctypes.byref(written)))
        return buf.value.decode()

    # ── pipeline control ──────────────────────────────────────────────────

    def pause(self) -> None:
        _check(self._lib.esccore_pause())

    def resume(self) -> None:
        _check(self._lib.esccore_resume())

    # ── context manager ───────────────────────────────────────────────────

    def __enter__(self) -> Engine:
        return self

    def __exit__(self, *_: object) -> None:
        if self._initialised:
            self.shutdown()

    # ── private ───────────────────────────────────────────────────────────

    @staticmethod
    def _make_event(eid: int, pri: Priority, topic: str, body: bytes) -> _EscEvent:
        evt = _EscEvent()
        evt.id = eid
        evt.priority = int(pri)
        evt.topic = topic.encode()
        if body:
            body_arr = (ctypes.c_uint8 * len(body))(*body)
            evt.body = ctypes.cast(body_arr, ctypes.POINTER(ctypes.c_uint8))
            evt.body_len = len(body)
        else:
            evt.body = None
            evt.body_len = 0
        return evt

    def _bind_functions(self) -> None:
        """Declare argument / return types for safety."""
        L = self._lib

        L.esccore_init.argtypes = [ctypes.c_char_p]
        L.esccore_init.restype = ctypes.c_int

        L.esccore_shutdown.argtypes = []
        L.esccore_shutdown.restype = ctypes.c_int

        L.esccore_push.argtypes = [ctypes.POINTER(_EscEvent)]
        L.esccore_push.restype = ctypes.c_int

        L.esccore_push_batch.argtypes = [
            ctypes.POINTER(_EscEvent), ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        L.esccore_push_batch.restype = ctypes.c_int

        L.esccore_metrics.argtypes = [ctypes.POINTER(_EscMetrics)]
        L.esccore_metrics.restype = ctypes.c_int

        L.esccore_health.argtypes = [ctypes.POINTER(_EscHealth)]
        L.esccore_health.restype = ctypes.c_int

        L.esccore_metrics_prometheus.argtypes = [
            ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
        ]
        L.esccore_metrics_prometheus.restype = ctypes.c_int

        L.esccore_pause.argtypes = []
        L.esccore_pause.restype = ctypes.c_int

        L.esccore_resume.argtypes = []
        L.esccore_resume.restype = ctypes.c_int
