"""Core engine wrapper — loads libesccore.so and exposes a Pythonic API.

Events are received via callbacks registered with Engine.subscribe().
There is no push path from Python into the engine; events originate
inside the C++ pipeline and flow out through registered subscribers.
"""

from __future__ import annotations

import ctypes
import os
import platform
from typing import Callable, Optional

from esccore.types import (
    Metrics, Health,
    _EscEvent, _EscMetrics, _EscHealth,
)


class EscError(RuntimeError):
    """Raised when a C API call returns a non-zero status."""

    _MESSAGES = {
        -1:  "Engine not initialised or already shut down",
        -3:  "Invalid argument",
        -99: "Internal engine error",
    }

    def __init__(self, code: int) -> None:
        self.code = code
        super().__init__(self._MESSAGES.get(code, f"Unknown error ({code})"))


def _check(rc: int) -> None:
    if rc != 0:
        raise EscError(rc)


# Callback type: (esc_event_t*, void*) -> int
_CB_TYPE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(_EscEvent), ctypes.c_void_p)


class Engine:
    """
    Python interface to the EventStreamCore engine.

    Usage::

        def on_event(event, user_data):
            print(event.topic.decode(), bytes(event.body[:event.body_len]))
            return 0  # keep receiving

        engine = Engine("/path/to/libesccore.so")
        engine.init("config/config.yaml")
        engine.subscribe("sensor/", on_event)
        # … engine runs; on_event is called for each processed event …
        engine.shutdown()
    """

    def __init__(self, lib_path: Optional[str] = None) -> None:
        default_lib = "libesccore.dll" if platform.system() == "Windows" else "libesccore.so"
        path = lib_path or os.environ.get("ESCCORE_LIB", default_lib)
        self._lib = ctypes.CDLL(path)
        self._bind_functions()
        self._initialised = False
        # Keep references to C callbacks alive so they are not GC-collected.
        self._callbacks: list[ctypes.CFUNCTYPE] = []

    # ── lifecycle ─────────────────────────────────────────────────────────

    def init(self, config_path: str = "config/config.yaml") -> None:
        _check(self._lib.esccore_init(config_path.encode()))
        self._initialised = True

    def shutdown(self) -> None:
        _check(self._lib.esccore_shutdown())
        self._initialised = False

    # ── subscribe ─────────────────────────────────────────────────────────

    def subscribe(
        self,
        topic_prefix: str = "",
        callback: Callable[[_EscEvent, object], int] = None,
        user_data: object = None,
    ) -> None:
        """Register *callback* to receive processed events from the engine.

        Args:
            topic_prefix: Only events whose topic starts with this string are
                          delivered.  Pass "" or None for all topics.
            callback:     Python callable with signature
                          ``(event: _EscEvent_ptr, user_data: c_void_p) -> int``.
                          Return 0 to keep receiving, non-zero to unsubscribe.
            user_data:    Opaque value forwarded to every callback invocation.
        """
        if callback is None:
            raise ValueError("callback must not be None")

        c_cb = _CB_TYPE(callback)
        self._callbacks.append(c_cb)  # prevent GC

        prefix_bytes = topic_prefix.encode() if topic_prefix else None
        _check(self._lib.esccore_subscribe(prefix_bytes, c_cb, user_data))

    # ── observability ─────────────────────────────────────────────────────

    def metrics(self) -> Metrics:
        m = _EscMetrics()
        _check(self._lib.esccore_metrics(ctypes.byref(m)))
        return Metrics._from_c(m)

    def health(self) -> Health:
        h = _EscHealth()
        _check(self._lib.esccore_health(ctypes.byref(h)))
        return Health._from_c(h)

    # ── context manager ───────────────────────────────────────────────────

    def __enter__(self) -> Engine:
        return self

    def __exit__(self, *_: object) -> None:
        if self._initialised:
            self.shutdown()

    # ── private ───────────────────────────────────────────────────────────

    def _bind_functions(self) -> None:
        L = self._lib

        L.esccore_init.argtypes = [ctypes.c_char_p]
        L.esccore_init.restype = ctypes.c_int

        L.esccore_shutdown.argtypes = []
        L.esccore_shutdown.restype = ctypes.c_int

        L.esccore_subscribe.argtypes = [
            ctypes.c_char_p,   # topic_prefix
            _CB_TYPE,          # callback
            ctypes.c_void_p,   # user_data
        ]
        L.esccore_subscribe.restype = ctypes.c_int

        L.esccore_metrics.argtypes = [ctypes.POINTER(_EscMetrics)]
        L.esccore_metrics.restype = ctypes.c_int

        L.esccore_health.argtypes = [ctypes.POINTER(_EscHealth)]
        L.esccore_health.restype = ctypes.c_int
