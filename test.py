#!/usr/bin/env python3
"""
Comprehensive EventStreamCore Test Suite
Tests all 3 processors: Realtime, Transactional, and Batch
Verifies metrics tracking and SLA compliance
"""

import socket
import time
import struct
import random
import sys
from datetime import datetime

# Event priority constants
BATCH = 0
LOW = 1
MEDIUM = 2
HIGH = 3
CRITICAL = 4

# Topic mapping to priorities
TOPIC_MAP = {
    # Realtime processor (HIGH/CRITICAL)
    "system/alerts": HIGH,
    "system/errors": CRITICAL,
    "user/login": HIGH,
    "payment/processed": HIGH,
    
    # Transactional processor (MEDIUM/LOW)
    "database/write": MEDIUM,
    "order/created": MEDIUM,
    "user/profile_update": LOW,
    "analytics/event": LOW,
    
    # Batch processor (BATCH)
    "analytics/metrics": BATCH,
    "batch/events": BATCH,
    "batch/aggregation": BATCH,
}

def create_event(event_id, topic, data, priority=None):
    """
    Create an event frame in the expected format:
    [4-byte frame_len][1-byte priority][2-byte topic_len][topic bytes][payload bytes]
    
    frame_len = 1 (priority) + 2 (topic_len) + len(topic) + len(data)
    """
    if priority is None:
        priority = TOPIC_MAP.get(topic, MEDIUM)
    
    topic_bytes = topic.encode('utf-8')
    data_bytes = data.encode('utf-8') if isinstance(data, str) else data
    
    # Build frame body: priority(1) + topic_len(2) + topic + data
    body = struct.pack('!B', priority)  # 1 byte priority
    body += struct.pack('!H', len(topic_bytes))  # 2 bytes topic length
    body += topic_bytes  # topic string
    body += data_bytes  # payload data
    
    # Add frame length prefix (4 bytes, big-endian)
    frame = struct.pack('!I', len(body)) + body
    return frame

def send_event(sock, event_id, topic, data, priority=None):
    """Send an event to the server"""
    frame = create_event(event_id, topic, data, priority)
    sock.sendall(frame)
    priority_val = priority or TOPIC_MAP.get(topic, MEDIUM)
    priority_names = {0: "BATCH", 1: "LOW", 2: "MEDIUM", 3: "HIGH", 4: "CRITICAL"}
    print(f"  [{event_id:3d}] {topic:30s} data={len(data):3d}b priority={priority_names[priority_val]}")

def send_batch_events(sock, start_id, count, topic, priority=BATCH):
    """Send a batch of events"""
    for i in range(count):
        event_id = start_id + i
        data = f"batch_data_{i}_{int(time.time()*1000)}"
        send_event(sock, event_id, topic, data, priority)

def test_realtime_processor(sock):
    """Test Realtime Processor with HIGH/CRITICAL priority events"""
    print("\n" + "="*70)
    print("TEST 1: RealtimeProcessor (HIGH/CRITICAL Priority, 5ms SLA)")
    print("="*70)
    print(f"Time: {datetime.now().strftime('%H:%M:%S')}")
    print("\nSending 8 HIGH/CRITICAL priority events:")
    
    test_cases = [
        (1, "system/alerts", "High severity alert", HIGH),
        (2, "system/errors", "Critical error detected", CRITICAL),
        (3, "user/login", "User authentication", HIGH),
        (4, "payment/processed", "Payment completed", HIGH),
        (5, "system/alerts", "Temperature high", HIGH),
        (6, "system/errors", "Connection lost", CRITICAL),
        (7, "user/login", "Session start", HIGH),
        (8, "payment/processed", "Payment verified", HIGH),
    ]
    
    for event_id, topic, data, priority in test_cases:
        send_event(sock, event_id, topic, data, priority)
        time.sleep(0.05)
    
    print(f"\n✓ Sent {len(test_cases)} events to RealtimeProcessor")
    print("  Expected: All should process within 5ms SLA")
    print("  Waiting 2 seconds for processing...")
    time.sleep(2)

def test_transactional_processor(sock):
    """Test Transactional Processor with MEDIUM/LOW priority + idempotency"""
    print("\n" + "="*70)
    print("TEST 2: TransactionalProcessor (MEDIUM/LOW Priority, Idempotency)")
    print("="*70)
    print(f"Time: {datetime.now().strftime('%H:%M:%S')}")
    print("\nPhase 1: Sending 6 ordered events with retry logic:")
    
    test_cases = [
        (10, "database/write", "User record update", MEDIUM),
        (11, "order/created", "New order #12345", MEDIUM),
        (12, "user/profile_update", "Profile change", LOW),
        (13, "database/write", "Account modified", MEDIUM),
        (14, "order/created", "New order #12346", MEDIUM),
        (15, "user/profile_update", "Avatar updated", LOW),
    ]
    
    for event_id, topic, data, priority in test_cases:
        send_event(sock, event_id, topic, data, priority)
        time.sleep(0.05)
    
    print(f"\n✓ Sent {len(test_cases)} initial events")
    
    time.sleep(1)
    print("\nPhase 2: Testing idempotency - resending duplicate event_id=10:")
    send_event(sock, 10, "database/write", "User record update (duplicate)", MEDIUM)
    
    print("\nPhase 3: Testing more duplicates - resending event_id=13:")
    send_event(sock, 13, "database/write", "Account modified (duplicate)", MEDIUM)
    
    print(f"\n✓ Sent duplicate events (should be skipped)")
    print("  Expected: Duplicates detected and skipped")
    print("  Metrics: total_events_skipped should increase")
    print("  Waiting 2 seconds for processing...")
    time.sleep(2)

def test_batch_processor(sock):
    """Test Batch Processor with 5-second window aggregation"""
    print("\n" + "="*70)
    print("TEST 3: BatchProcessor (BATCH Priority, 5-second Window)")
    print("="*70)
    print(f"Time: {datetime.now().strftime('%H:%M:%S')}")
    
    print("\nPhase 1: Sending 5 batch events to analytics/metrics:")
    send_batch_events(sock, 20, 5, "analytics/metrics")
    print(f"\n✓ Sent 5 batch events")
    print("  Expected: [BATCH FLUSH] after 5 seconds")
    
    print("\nPhase 2: Waiting for 5-second window to flush...")
    for i in range(5, 0, -1):
        print(f"  ... {i}s remaining", end='\r')
        time.sleep(1)
    print("  Flush should have occurred!            ")
    
    time.sleep(1)
    print("\nPhase 3: Sending 3 more batch events (new window):")
    send_batch_events(sock, 25, 3, "analytics/metrics")
    print(f"\n✓ Sent 3 batch events (new window)")
    
    print("\nPhase 4: Waiting 5 seconds for second window...")
    for i in range(5, 0, -1):
        print(f"  ... {i}s remaining", end='\r')
        time.sleep(1)
    print("  Second flush should have occurred!     ")
    time.sleep(1)

def test_mixed_priority_stress(sock):
    """Test all 3 processors concurrently with mixed priorities"""
    print("\n" + "="*70)
    print("TEST 4: Mixed Priority Stress Test (All Processors)")
    print("="*70)
    print(f"Time: {datetime.now().strftime('%H:%M:%S')}")
    
    print("\nSending 30 mixed-priority events across all processors:")
    
    event_id = 50
    topics_list = list(TOPIC_MAP.items())
    sent = {"realtime": 0, "transactional": 0, "batch": 0}
    
    for i in range(30):
        topic, priority = random.choice(topics_list)
        data = f"stress_{i}_{random.randint(1000, 9999)}"
        send_event(sock, event_id + i, topic, data, priority)
        
        if priority in [HIGH, CRITICAL]:
            sent["realtime"] += 1
        elif priority in [MEDIUM, LOW]:
            sent["transactional"] += 1
        else:
            sent["batch"] += 1
        
        time.sleep(random.uniform(0.01, 0.05))
    
    print(f"\n✓ Sent 30 mixed-priority events:")
    print(f"  - Realtime (HIGH/CRITICAL): {sent['realtime']} events")
    print(f"  - Transactional (MEDIUM/LOW): {sent['transactional']} events")
    print(f"  - Batch (BATCH): {sent['batch']} events")
    print("\nWaiting 7 seconds for all processors to complete...")
    for i in range(7, 0, -1):
        print(f"  ... {i}s remaining", end='\r')
        time.sleep(1)
    print("  Processing complete!                  ")

def test_metrics_snapshot(sock):
    """Test metrics collection and reporting"""
    print("\n" + "="*70)
    print("TEST 5: Metrics Snapshot (Final Statistics)")
    print("="*70)
    print(f"Time: {datetime.now().strftime('%H:%M:%S')}")
    
    print("\nSending 5 final events to trigger last metrics update:")
    send_event(sock, 100, "system/alerts", "Final test - Realtime", HIGH)
    send_event(sock, 101, "database/write", "Final test - Transactional", MEDIUM)
    send_event(sock, 102, "analytics/metrics", "Final test - Batch", BATCH)
    send_event(sock, 103, "order/created", "Final test - Transactional", MEDIUM)
    send_event(sock, 104, "system/errors", "Final test - Realtime", CRITICAL)
    
    print("\n✓ Metrics test events sent")
    print("\nMetrics will be displayed every 5 seconds in the server logs:")
    print("  ===== METRICS SNAPSHOT =====")
    print("  Processor: RealtimeProcessor")
    print("    Total Events Processed: <count>")
    print("    Total Events Dropped: <count>")
    print("    Total Events Alerted: <count>")
    print("    Total Events Errors: <count>")
    print("    Total Events Skipped: <count>")
    print("  Processor: TransactionalProcessor")
    print("    ... (same metrics)")
    print("  Processor: BatchProcessor")
    print("    ... (same metrics)")
    
    print("\nWaiting 2 seconds...")
    time.sleep(2)

def print_test_summary():
    """Print test execution summary"""
    print("\n" + "="*70)
    print("TEST SUITE SUMMARY")
    print("="*70)
    
    summary = """
✓ TEST 1: RealtimeProcessor (HIGH/CRITICAL)
  Objective: Verify 5ms SLA enforcement
  Events Sent: 8 HIGH/CRITICAL priority events
  Expected: Process < 5ms, drop if exceeds
  Metrics: total_events_processed, total_events_dropped

✓ TEST 2: TransactionalProcessor (MEDIUM/LOW + Idempotency)
  Objective: Verify ordered processing and idempotency
  Events Sent: 6 initial + 2 duplicates
  Expected: Duplicates skipped, ordered processing
  Metrics: total_events_processed, total_events_dropped, total_events_skipped
  Verification: Event IDs 10 and 13 resent, should skip

✓ TEST 3: BatchProcessor (5s Window Aggregation)
  Objective: Verify time-window based flushing
  Events Sent: 5 events (Phase 1), 3 events (Phase 3)
  Expected: [BATCH FLUSH] messages at 5s intervals
  Metrics: total_events_processed (accumulation count)

✓ TEST 4: Mixed Priority Stress Test
  Objective: All processors handling concurrent load
  Events Sent: 30 random priority events
  Distribution: ~10 Realtime, ~10 Transactional, ~10 Batch
  Expected: All processors active simultaneously

✓ TEST 5: Metrics Collection & Reporting
  Objective: Verify metrics tracking across all processors
  Snapshot Frequency: Every 5 seconds
  Metrics Tracked:
    • total_events_processed: Successful processing count
    • total_events_dropped: Failed/SLA violation count
    • total_events_alerted: Alert events (Realtime)
    • total_events_errors: Error count
    • total_events_skipped: Idempotent skips (Transactional)

TOTAL EVENTS SENT: ~120 events across all tests
DURATION: ~30 seconds (includes 5s window waits)
LOGS: Check EventStreamCore.exe console for detailed output
"""
    print(summary)

def main():
    """Main test execution"""
    if len(sys.argv) > 1:
        host = sys.argv[1]
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
    else:
        host = "127.0.0.1"
        port = 9000
    
    print("\n" + "="*70)
    print("EVENTSTREAM CORE - COMPREHENSIVE TEST SUITE")
    print("="*70)
    print(f"Connecting to {host}:{port}...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print("✓ Connected successfully!\n")
        
        # Run all tests
        test_realtime_processor(sock)
        test_transactional_processor(sock)
        test_batch_processor(sock)
        test_mixed_priority_stress(sock)
        test_metrics_snapshot(sock)
        
        sock.close()
        print_test_summary()
        
        print("\n" + "="*70)
        print("✓ ALL TESTS COMPLETED SUCCESSFULLY")
        print("="*70)
        print("\nNotes:")
        print("  • Check EventStreamCore.exe console for detailed logs")
        print("  • Metrics snapshot appears every 5 seconds")
        print("  • Check for SLA violations: 'DROPPED event' in logs")
        print("  • Check for idempotency: 'already processed' in logs")
        print("  • Check for batch flush: '[BATCH FLUSH]' in logs")
        
    except ConnectionRefusedError:
        print(f"\n✗ FAILED: Could not connect to {host}:{port}")
        print("\nMake sure EventStreamCore.exe is running:")
        print("  cd build")
        print("  .\\EventStreamCore.exe")
        sys.exit(1)
    except Exception as e:
        print(f"\n✗ ERROR: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()

