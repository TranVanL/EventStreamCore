"""Data types shared between the SDK and the adapter."""

from __future__ import annotations

import ctypes
import enum
from dataclasses import dataclass


# ── Priority mirrors esc_priority_t ──────────────────────────────────────────

class Priority(enum.IntEnum):
    BATCH    = 0
    LOW      = 1
    MEDIUM   = 2
    HIGH     = 3
    CRITICAL = 4


# ── ctypes structs (match esccore.h exactly) ─────────────────────────────────

class _EscEvent(ctypes.Structure):
    _fields_ = [
        ("id",       ctypes.c_uint32),
        ("priority", ctypes.c_int),      # esc_priority_t
        ("topic",    ctypes.c_char_p),
        ("body",     ctypes.POINTER(ctypes.c_uint8)),
        ("body_len", ctypes.c_size_t),
    ]


class _EscMetrics(ctypes.Structure):
    _fields_ = [
        ("total_events_pushed",        ctypes.c_uint64),
        ("total_events_processed",     ctypes.c_uint64),
        ("total_events_dropped",       ctypes.c_uint64),
        ("realtime_queue_depth",       ctypes.c_uint64),
        ("transactional_queue_depth",  ctypes.c_uint64),
        ("batch_queue_depth",          ctypes.c_uint64),
        ("dlq_depth",                  ctypes.c_uint64),
        ("backpressure_level",         ctypes.c_int),
    ]


class _EscHealth(ctypes.Structure):
    _fields_ = [
        ("is_alive",           ctypes.c_int),
        ("is_ready",           ctypes.c_int),
        ("backpressure_level", ctypes.c_int),
    ]


# ── Pythonic data classes returned to callers ────────────────────────────────

@dataclass(frozen=True, slots=True)
class Metrics:
    total_events_pushed: int
    total_events_processed: int
    total_events_dropped: int
    realtime_queue_depth: int
    transactional_queue_depth: int
    batch_queue_depth: int
    dlq_depth: int
    backpressure_level: int

    @classmethod
    def _from_c(cls, c: _EscMetrics) -> Metrics:
        return cls(
            total_events_pushed=c.total_events_pushed,
            total_events_processed=c.total_events_processed,
            total_events_dropped=c.total_events_dropped,
            realtime_queue_depth=c.realtime_queue_depth,
            transactional_queue_depth=c.transactional_queue_depth,
            batch_queue_depth=c.batch_queue_depth,
            dlq_depth=c.dlq_depth,
            backpressure_level=c.backpressure_level,
        )


@dataclass(frozen=True, slots=True)
class Health:
    is_alive: bool
    is_ready: bool
    backpressure_level: int

    @classmethod
    def _from_c(cls, c: _EscHealth) -> Health:
        return cls(
            is_alive=bool(c.is_alive),
            is_ready=bool(c.is_ready),
            backpressure_level=c.backpressure_level,
        )
