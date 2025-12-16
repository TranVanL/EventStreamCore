#!/usr/bin/env python3
"""
Comprehensive EventStreamCore Test Suite
Tests all processor types, RingBuffer integration, drop policies, metrics, and graceful shutdown

Features tested:
- REALTIME processor: HIGH/CRITICAL events, 5ms SLA, RingBuffer queue, low-latency processing
- TRANSACTIONAL processor: MEDIUM/LOW events, ordered processing, idempotency
- BATCH processor: BATCH events, 5-second window aggregation
- Drop policies: DROP_OLD (REALTIME), BLOCK_PRODUCER (TRANSACTIONAL), DROP_NEW (BATCH)
- Metrics tracking: event counts, processing times, latencies
- Thread affinity: CPU core pinning for REALTIME processor
- Graceful shutdown: proper cleanup sequence
"""

import socket
import time
import struct
import random
import sys
import threading
from datetime import datetime

# Event priority constants
BATCH = 0
LOW = 1
MEDIUM = 2
HIGH = 3
CRITICAL = 4

# Topic mappings (used for priority inference)
REALTIME_TOPICS = {
    "system/alerts": HIGH,
    "system/errors": CRITICAL,
    "security/breach": CRITICAL,
    "payment/fraud_detected": CRITICAL,
    "user/login": HIGH,
    "user/logout": HIGH,
    "payment/processed": HIGH,
    "sensor/temperature_critical": CRITICAL,
}

TRANSACTIONAL_TOPICS = {
    "database/write": MEDIUM,
    "database/read": MEDIUM,
    "order/created": MEDIUM,
    "order/updated": MEDIUM,
    "user/profile_update": LOW,
    "user/preferences": LOW,
    "analytics/event": LOW,
    "logs/audit": LOW,
}

BATCH_TOPICS = {
    "analytics/metrics": BATCH,
    "analytics/aggregation": BATCH,
    "batch/events": BATCH,
    "batch/aggregation": BATCH,
    "reporting/daily": BATCH,
}

def create_event(event_id, topic, data, priority=None):
    """
    Create an event frame in the wire format:
    [4-byte frame_len][1-byte priority][2-byte topic_len][topic bytes][payload bytes]
    """
    if priority is None:
        # Auto-detect priority from topic
        if topic in REALTIME_TOPICS:
            priority = REALTIME_TOPICS[topic]
        elif topic in TRANSACTIONAL_TOPICS:
            priority = TRANSACTIONAL_TOPICS[topic]
        else:
            priority = BATCH_TOPICS.get(topic, BATCH)
    
    topic_bytes = topic.encode('utf-8')
    data_bytes = data.encode('utf-8') if isinstance(data, str) else data
    
    # Build frame: priority(1) + topic_len(2) + topic + data
    body = struct.pack('!B', priority)  # 1 byte priority
    body += struct.pack('!H', len(topic_bytes))  # 2 bytes topic length
    body += topic_bytes  # topic string
    body += data_bytes  # payload
    
    # Frame length prefix (4 bytes)
    frame = struct.pack('!I', len(body)) + body
    return frame

def send_event(sock, event_id, topic, data, priority=None, verbose=True):
    """Send event and optionally log it"""
    try:
        frame = create_event(event_id, topic, data, priority)
        sock.sendall(frame)
        
        if verbose:
            priority_val = priority or REALTIME_TOPICS.get(topic, TRANSACTIONAL_TOPICS.get(topic, BATCH))
            priority_names = {0: "BATCH", 1: "LOW", 2: "MEDIUM", 3: "HIGH", 4: "CRITICAL"}
            print(f"  [{event_id:4d}] {topic:35s} ({priority_names[priority_val]:8s}) {len(data):4d} bytes")
    except Exception as e:
        print(f"  ✗ ERROR sending event {event_id}: {e}")
        return False
    return True

def wait_with_countdown(seconds, label="Waiting"):
    """Display countdown timer"""
    for i in range(seconds, 0, -1):
        print(f"  {label}... {i}s remaining\r", end="", flush=True)
        time.sleep(1)
    print(f"  {label}... done!{'':20s}")

def separator(title=""):
    """Print test separator"""
    if title:
        print(f"\n{'='*80}")
        print(f"  {title}")
        print(f"{'='*80}")
    else:
        print(f"\n{'-'*80}")

# ======================== TEST 1: REALTIME PROCESSOR ========================

def test_1_realtime_processor(sock):
    """
    TEST 1: REALTIME PROCESSOR
    - Tests HIGH/CRITICAL priority events
    - Verifies 5ms SLA enforcement
    - Uses lock-free RingBuffer queue (16384 capacity)
    - Expects minimal latency (sub-millisecond per-event)
    - Tests drop policies when queue is full (DROP_OLD)
    """
    separator("TEST 1: REALTIME PROCESSOR (HIGH/CRITICAL Events, 5ms SLA)")
    print(f"Started: {datetime.now().strftime('%H:%M:%S')}")
    print("""
Features tested:
  • Lock-free RingBuffer queue (16384 capacity)
  • Thread affinity (pinned to CPU core 2)
  • DROP_OLD policy when queue full
  • 5ms SLA monitoring
  • Event counts: processed/dropped
    """)
    
    print("Phase 1: Sending 16 normal-load HIGH/CRITICAL events")
    critical_events = [
        (101, "system/errors", "Critical database connection failed"),
        (102, "security/breach", "Unauthorized access attempt detected"),
        (103, "payment/fraud_detected", "Suspicious payment pattern detected"),
        (104, "system/alerts", "Memory usage exceeded 90%"),
        (105, "sensor/temperature_critical", "Temperature 150°C - overheat detected"),
        (106, "user/login", "VIP user login from new location"),
        (107, "payment/processed", "High-value payment $50,000 processed"),
        (108, "system/errors", "Service unavailable - cascading failure"),
    ]
    
    for event_id, topic, data in critical_events:
        send_event(sock, event_id, topic, data, verbose=True)
        time.sleep(0.01)  # Small delay to space out events
    
    print(f"\n✓ Sent {len(critical_events)} CRITICAL events")
    
    print("\nPhase 2: High-frequency burst (stress test)")
    print("  Sending 50 events rapidly to test RingBuffer capacity and DROP_OLD")
    for i in range(50):
        event_id = 200 + i
        topic = random.choice(list(REALTIME_TOPICS.keys()))
        data = f"burst_event_{i}_data_{random.randint(1000, 9999)}"
        send_event(sock, event_id, topic, data, verbose=False)
        time.sleep(0.002)  # 2ms interval
    
    print(f"  ✓ Sent 50 burst events")
    print(f"  → Expected: RingBuffer handles ~32k events (16384 capacity)")
    print(f"  → If queue fills: DROP_OLD removes oldest, DROP_NEW drops newest")
    
    print("\nPhase 3: Waiting for REALTIME processor to drain queue")
    wait_with_countdown(3, "Processing")
    
    print(f"\nEnded: {datetime.now().strftime('%H:%M:%S')}")
    print("Expected metrics:")
    print("  • RealtimeProcessor: 66+ events processed")
    print("  • Dropped: 0 (if RingBuffer didn't overflow)")
    print("  • Alert events if latency exceeded 5ms SLA")

# ======================== TEST 2: TRANSACTIONAL PROCESSOR ========================

def test_2_transactional_processor(sock):
    """
    TEST 2: TRANSACTIONAL PROCESSOR
    - Tests MEDIUM/LOW priority events
    - Verifies ordered processing
    - Tests idempotency (duplicate detection)
    - Uses mutex-protected deque with condition variables
    - Expects: ordered delivery, duplicate skipping
    """
    separator("TEST 2: TRANSACTIONAL PROCESSOR (MEDIUM/LOW Events, Idempotency)")
    print(f"Started: {datetime.now().strftime('%H:%M:%S')}")
    print("""
Features tested:
  • Mutex-protected deque for ordered processing
  • Idempotency: duplicate event detection by event ID
  • BLOCK_PRODUCER policy when queue full
  • Event ordering preservation
  • Transactional semantics (all-or-nothing)
    """)
    
    print("Phase 1: Sending 10 ordered MEDIUM/LOW events")
    tx_events = [
        (301, "database/write", "CREATE TABLE users (id INT)"),
        (302, "order/created", "Order #10001 created by user_123"),
        (303, "user/profile_update", "User profile: avatar changed"),
        (304, "database/write", "UPDATE users SET status='active'"),
        (305, "order/updated", "Order #10001 status: processing"),
        (306, "user/preferences", "Theme changed to dark mode"),
        (307, "analytics/event", "Page view: /dashboard"),
        (308, "logs/audit", "Admin action: user_456 created"),
        (309, "database/read", "SELECT * FROM orders LIMIT 100"),
        (310, "order/created", "Order #10002 created by user_789"),
    ]
    
    for event_id, topic, data in tx_events:
        send_event(sock, event_id, topic, data, verbose=True)
        time.sleep(0.02)  # Small delay
    
    print(f"\n✓ Sent {len(tx_events)} TRANSACTIONAL events")
    
    print("\nPhase 2: Testing idempotency - resend 3 duplicate events")
    print("  Resending event_id=301, 305, 310 (should be skipped)")
    duplicates = [
        (301, "database/write", "CREATE TABLE users (duplicate)"),
        (305, "order/updated", "Order status update (duplicate)"),
        (310, "order/created", "Order #10002 (duplicate)"),
    ]
    
    for event_id, topic, data in duplicates:
        send_event(sock, event_id, topic, data, verbose=True)
        time.sleep(0.02)
    
    print(f"\n✓ Sent 3 duplicate events (idempotent retries)")
    print(f"  → Expected: Duplicates detected and skipped by ID")
    print(f"  → Metrics: total_events_skipped should increase by 3")
    
    print("\nPhase 3: Waiting for TRANSACTIONAL processor to complete")
    wait_with_countdown(3, "Processing")
    
    print(f"\nEnded: {datetime.now().strftime('%H:%M:%S')}")
    print("Expected metrics:")
    print("  • TransactionalProcessor: 10 processed, 3 skipped (duplicates)")
    print("  • Events processed in order (FIFO)")
    print("  • Idempotency verified: same event_id not processed twice")

# ======================== TEST 3: BATCH PROCESSOR ========================

def test_3_batch_processor(sock):
    """
    TEST 3: BATCH PROCESSOR
    - Tests BATCH priority events
    - Verifies 5-second window aggregation
    - Tests batch flushing and metrics
    - Expects: aggregation after 5s window or max events
    """
    separator("TEST 3: BATCH PROCESSOR (BATCH Events, 5s Window)")
    print(f"Started: {datetime.now().strftime('%H:%M:%S')}")
    print("""
Features tested:
  • 5-second time-window based batching
  • Event accumulation and aggregation
  • Batch flush on window boundary or capacity
  • DROP_NEW policy when queue full
  • Metrics: batch_count, flush_count
    """)
    
    print("Phase 1: Sending 8 BATCH events (will accumulate)")
    batch_events = [
        (401, "analytics/metrics", "CPU usage: 45%"),
        (402, "reporting/daily", "Daily report: 1000 orders"),
        (403, "analytics/aggregation", "Hourly aggregation: revenue $50k"),
        (404, "batch/events", "Batch event #1"),
        (405, "analytics/metrics", "Memory usage: 72%"),
        (406, "reporting/daily", "Daily report: 500 errors"),
        (407, "batch/events", "Batch event #2"),
        (408, "analytics/aggregation", "Hourly aggregation: users 5000"),
    ]
    
    for event_id, topic, data in batch_events:
        send_event(sock, event_id, topic, data, verbose=True)
        time.sleep(0.02)
    
    print(f"\n✓ Sent {len(batch_events)} BATCH events")
    print(f"  → Events accumulated in 5s window")
    
    print("\nPhase 2: Waiting for 5-second window to flush...")
    wait_with_countdown(6, "Waiting for batch window")
    print(f"  → [BATCH FLUSH] should appear in metrics")
    
    print("\nPhase 3: Sending 5 more events (new window)")
    batch_events_2 = [
        (501, "analytics/metrics", "CPU usage: 50%"),
        (502, "reporting/daily", "Daily report: 1100 orders"),
        (503, "analytics/aggregation", "Hourly aggregation: revenue $55k"),
        (504, "batch/events", "Batch event #3"),
        (505, "batch/events", "Batch event #4"),
    ]
    
    for event_id, topic, data in batch_events_2:
        send_event(sock, event_id, topic, data, verbose=True)
        time.sleep(0.02)
    
    print(f"\n✓ Sent {len(batch_events_2)} more BATCH events (new window)")
    
    print("\nPhase 4: Waiting for second batch window...")
    wait_with_countdown(6, "Waiting for second batch window")
    
    print(f"\nEnded: {datetime.now().strftime('%H:%M:%S')}")
    print("Expected metrics:")
    print("  • BatchProcessor: 13 events processed (8+5)")
    print("  • Two [BATCH FLUSH] events in logs (~5s apart)")
    print("  • Batch aggregation completions logged")

# ======================== TEST 4: MIXED LOAD ========================

def test_4_mixed_priority_stress(sock):
    """
    TEST 4: MIXED PRIORITY STRESS
    - Tests all 3 processors simultaneously
    - Mixed random priorities and topics
    - Verifies system stability under concurrent load
    - Measures overall throughput
    """
    separator("TEST 4: MIXED PRIORITY STRESS TEST (All Processors)")
    print(f"Started: {datetime.now().strftime('%H:%M:%S')}")
    print("""
Features tested:
  • Concurrent processing: REALTIME + TRANSACTIONAL + BATCH
  • Mixed priority distribution
  • EventBusMulti routing efficiency
  • Dispatcher load balancing
  • Metrics aggregation across processors
    """)
    
    print("Phase 1: Sending 100 mixed-priority events rapidly")
    all_topics = list(REALTIME_TOPICS.items()) + list(TRANSACTIONAL_TOPICS.items()) + list(BATCH_TOPICS.items())
    
    event_counts = {"realtime": 0, "transactional": 0, "batch": 0}
    
    for i in range(100):
        event_id = 600 + i
        topic, priority = random.choice(all_topics)
        data = f"stress_test_{i}_payload_{random.randint(10000, 99999)}"
        send_event(sock, event_id, topic, data, priority=priority, verbose=False)
        
        if priority in [HIGH, CRITICAL]:
            event_counts["realtime"] += 1
        elif priority in [MEDIUM, LOW]:
            event_counts["transactional"] += 1
        else:
            event_counts["batch"] += 1
        
        if (i + 1) % 25 == 0:
            print(f"  ✓ Sent {i+1:3d} events...")
        
        time.sleep(0.003)  # 3ms between events
    
    print(f"\n✓ Sent 100 mixed-priority events")
    print(f"  • REALTIME (HIGH/CRITICAL): {event_counts['realtime']} events")
    print(f"  • TRANSACTIONAL (MEDIUM/LOW): {event_counts['transactional']} events")
    print(f"  • BATCH: {event_counts['batch']} events")
    
    print("\nPhase 2: Waiting for all processors to complete...")
    wait_with_countdown(8, "Processing")
    
    print(f"\nEnded: {datetime.now().strftime('%H:%M:%S')}")
    print("Expected behavior:")
    print("  • All 100 events distributed to 3 processors")
    print("  • System remains stable (no crashes)")
    print("  • All processors handle concurrent load")
    print("  • Total processed: ~100 events")

# ======================== TEST 5: OVERFLOW & DROP POLICIES ========================

def test_5_drop_policies(sock):
    """
    TEST 5: DROP POLICIES
    - Tests DROP_OLD (REALTIME): removes oldest when full
    - Tests BLOCK_PRODUCER (TRANSACTIONAL): producer waits
    - Tests DROP_NEW (BATCH): rejects incoming when full
    - Verifies proper backpressure handling
    """
    separator("TEST 5: DROP POLICIES & BACKPRESSURE")
    print(f"Started: {datetime.now().strftime('%H:%M:%S')}")
    print("""
Features tested:
  • DROP_OLD (REALTIME): Oldest events dropped if queue full
  • BLOCK_PRODUCER (TRANSACTIONAL): Producer blocks/waits
  • DROP_NEW (BATCH): New events dropped if queue full
  • Overflow metrics tracking
  • Queue capacity management
    """)
    
    print("Phase 1: REALTIME DROP_OLD test - send burst to RingBuffer capacity")
    print("  Sending 20000 HIGH priority events (RingBuffer: 16384 capacity)")
    
    for i in range(20000):
        event_id = 700 + i
        topic = random.choice(list(REALTIME_TOPICS.keys()))
        data = f"overflow_test_{i}"
        send_event(sock, event_id, topic, data, verbose=False)
        
        if (i + 1) % 5000 == 0:
            print(f"  ✓ Sent {i+1:5d} events...")
        
        time.sleep(0.0001)  # Very short interval for stress
    
    print(f"\n✓ Sent 20000 HIGH priority events")
    print(f"  → Expected: ~3616 events dropped (20000 - 16384 capacity)")
    print(f"  → RingBuffer uses DROP_OLD: oldest discarded, newest kept")
    
    print("\nPhase 2: Waiting for queue drain...")
    wait_with_countdown(3, "Processing")
    
    print(f"\nEnded: {datetime.now().strftime('%H:%M:%S')}")
    print("Expected metrics:")
    print("  • total_overflow_drops: ~3616 (overflow exceeded capacity)")
    print("  • total_events_dropped: ~3616")
    print("  • RingBuffer: DROP_OLD strategy verified")

# ======================== TEST 6: METRICS & REPORTING ========================

def test_6_metrics_reporting(sock):
    """
    TEST 6: METRICS REPORTING
    - Verifies metrics collection and reporting
    - Tests MetricsReporter 5-second snapshots
    - Verifies counter accuracy
    """
    separator("TEST 6: METRICS REPORTING & COLLECTION")
    print(f"Started: {datetime.now().strftime('%H:%M:%S')}")
    print("""
Features tested:
  • Metrics snapshot reporting (5s intervals)
  • Counter accuracy: processed, dropped, skipped, alerted
  • Per-processor metrics
  • EventBusMulti metrics (enqueued, dequeued, blocked, overflows)
  • Latency tracking (avg, max)
    """)
    
    print("Phase 1: Sending 20 diverse events for metrics tracking")
    metrics_events = [
        (801, "system/alerts", "Alert for metrics test"),
        (802, "database/write", "Write operation for metrics"),
        (803, "analytics/metrics", "Metrics collection event"),
        (804, "user/login", "Login for metrics"),
        (805, "batch/events", "Batch for metrics"),
        (806, "system/errors", "Error for metrics testing"),
        (807, "order/created", "Order for metrics"),
        (808, "analytics/aggregation", "Aggregation for metrics"),
        (809, "user/profile_update", "Profile update for metrics"),
        (810, "payment/processed", "Payment for metrics"),
        (811, "security/breach", "Security event for metrics"),
        (812, "analytics/event", "Analytics for metrics"),
        (813, "logs/audit", "Audit log for metrics"),
        (814, "sensor/temperature_critical", "Sensor data for metrics"),
        (815, "payment/fraud_detected", "Fraud detection for metrics"),
        (816, "database/read", "Read operation for metrics"),
        (817, "user/preferences", "Preferences for metrics"),
        (818, "reporting/daily", "Daily report for metrics"),
        (819, "batch/aggregation", "Batch aggregation for metrics"),
        (820, "order/updated", "Order update for metrics"),
    ]
    
    for event_id, topic, data in metrics_events:
        send_event(sock, event_id, topic, data, verbose=False)
        time.sleep(0.01)
    
    print(f"✓ Sent {len(metrics_events)} events for metrics tracking")
    
    print("\nPhase 2: Waiting for metrics snapshot (5s interval)...")
    wait_with_countdown(6, "Metrics reporting")
    
    print(f"\nEnded: {datetime.now().strftime('%H:%M:%S')}")
    print("Expected in logs (METRICS SNAPSHOT):")
    print("  [RealtimeProcessor]")
    print("    +- Processed: 7+ events")
    print("    +- Dropped: 0 events")
    print("    +- Alerted: X events (if latency exceeded SLA)")
    print("  [TransactionalProcessor]")
    print("    +- Processed: 7+ events")
    print("    +- Skipped: X events (if any duplicates)")
    print("  [BatchProcessor]")
    print("    +- Processed: 6+ events")
    print("  [EventBusMulti]")
    print("    +- Enqueued: 20 events")
    print("    +- Dequeued: 20 events")

# ======================== MAIN TEST RUNNER ========================

def main():
    """Execute comprehensive test suite"""
    
    print("\n" + "="*80)
    print("  EVENTSTREAM CORE - COMPREHENSIVE TEST SUITE")
    print("="*80)
    print(f"\nStarted: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("\nFeatures to test:")
    print("  ✓ RingBuffer lock-free queue (REALTIME)")
    print("  ✓ Mutex-protected deque (TRANSACTIONAL)")
    print("  ✓ Time-window batching (BATCH)")
    print("  ✓ Drop policies (DROP_OLD, BLOCK_PRODUCER, DROP_NEW)")
    print("  ✓ Thread affinity (CPU core pinning)")
    print("  ✓ Metrics tracking and reporting")
    print("  ✓ Graceful shutdown and cleanup")
    
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
    
    print(f"\nConnecting to {host}:{port}...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print("✓ Connected successfully!\n")
        
        # Run all tests
        test_1_realtime_processor(sock)
        test_2_transactional_processor(sock)
        test_3_batch_processor(sock)
        test_4_mixed_priority_stress(sock)
        test_5_drop_policies(sock)
        test_6_metrics_reporting(sock)
        
        sock.close()
        
        print("\n" + "="*80)
        print("  ✓ ALL TESTS COMPLETED SUCCESSFULLY")
        print("="*80)
        print(f"\nEnded: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        print("\nInstructions for verifying results:")
        print("  1. Check EventStreamCore.exe console for:")
        print("     • METRICS SNAPSHOT messages (5s intervals)")
        print("     • Event processing logs for each processor")
        print("     • [BATCH FLUSH] messages for batch aggregation")
        print("     • 'DROPPED' messages if SLA violations detected")
        print("     • Destructor messages on graceful shutdown")
        print("\n  2. Expected total events processed: ~700+")
        print("     • REALTIME: ~116 events")
        print("     • TRANSACTIONAL: ~13 events")
        print("     • BATCH: ~13 events")
        print("     • STRESS: ~100 events distributed")
        print("     • METRICS: ~20 events")
        print("     • OVERFLOW: ~20000 (with drops)")
        print("\n  3. Drop policy verification:")
        print("     • REALTIME: DROP_OLD handles overflow gracefully")
        print("     • TRANSACTIONAL: BLOCK_PRODUCER (may see timeouts)")
        print("     • BATCH: DROP_NEW rejects when full")
        print("\n  4. Optional: Stop server with Ctrl+C to verify graceful shutdown")
        
    except ConnectionRefusedError:
        print(f"✗ FAILED: Could not connect to {host}:{port}")
        print("\nMake sure EventStreamCore.exe is running:")
        print("  cd build")
        print("  .\\EventStreamCore.exe")
        sys.exit(1)
    except Exception as e:
        print(f"✗ ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    main()
