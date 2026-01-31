#!/usr/bin/env python3
"""
Stress Test for EventStreamCore
Simulates high load with multiple concurrent TCP clients

Usage: python3 stress_test.py [host] [port] [clients] [events_per_client]
"""

import socket
import struct
import time
import json
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed

# Statistics
stats_lock = threading.Lock()
total_sent = 0
total_errors = 0
start_time = 0


def create_event_frame(event_id: int, topic: str, priority: int = 2) -> bytes:
    """Create event frame"""
    topic_bytes = topic.encode('utf-8')
    payload = json.dumps({"id": event_id, "ts": int(time.time()*1000)})
    payload_bytes = payload.encode('utf-8')
    
    frame_body = struct.pack('B', priority)
    frame_body += struct.pack('>H', len(topic_bytes))
    frame_body += topic_bytes
    frame_body += payload_bytes
    
    return struct.pack('>I', len(frame_body)) + frame_body


def client_worker(client_id: int, host: str, port: int, num_events: int) -> dict:
    """Worker function for each client"""
    global total_sent, total_errors
    
    sent = 0
    errors = 0
    client_start = time.time()
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30)
        sock.connect((host, port))
        
        topics = ["stress.realtime", "stress.transaction", "stress.batch"]
        
        for i in range(num_events):
            event_id = client_id * 100000 + i
            topic = topics[i % 3]
            priority = i % 5
            
            frame = create_event_frame(event_id, topic, priority)
            sock.sendall(frame)
            sent += 1
        
        sock.close()
        
    except Exception as e:
        errors = num_events - sent
    
    client_elapsed = time.time() - client_start
    
    with stats_lock:
        total_sent += sent
        total_errors += errors
    
    return {
        "client_id": client_id,
        "sent": sent,
        "errors": errors,
        "elapsed": client_elapsed
    }


def run_stress_test(host: str, port: int, num_clients: int, events_per_client: int):
    """Run stress test"""
    global total_sent, total_errors, start_time
    
    total_events = num_clients * events_per_client
    
    print(f"╔{'═'*70}╗")
    print(f"║  EVENTSTREAM STRESS TEST                                             ║")
    print(f"╠{'═'*70}╣")
    print(f"║  Target: {host}:{port}")
    print(f"║  Clients: {num_clients}")
    print(f"║  Events/client: {events_per_client}")
    print(f"║  Total events: {total_events}")
    print(f"╚{'═'*70}╝\n")
    
    total_sent = 0
    total_errors = 0
    start_time = time.time()
    
    # Use ThreadPoolExecutor for concurrent clients
    with ThreadPoolExecutor(max_workers=num_clients) as executor:
        futures = []
        for i in range(num_clients):
            future = executor.submit(client_worker, i, host, port, events_per_client)
            futures.append(future)
        
        # Wait for all to complete
        results = []
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            print(f"  Client {result['client_id']:3d}: sent={result['sent']:5d}, errors={result['errors']:3d}, time={result['elapsed']:.2f}s")
    
    elapsed = time.time() - start_time
    throughput = total_sent / elapsed if elapsed > 0 else 0
    error_rate = (total_errors / total_events * 100) if total_events > 0 else 0
    
    print(f"\n{'='*70}")
    print(f"📊 STRESS TEST RESULTS")
    print(f"{'='*70}")
    print(f"  Total Sent:     {total_sent:,} events")
    print(f"  Total Errors:   {total_errors:,} ({error_rate:.2f}%)")
    print(f"  Total Time:     {elapsed:.2f}s")
    print(f"  Throughput:     {throughput:,.0f} events/sec")
    print(f"{'='*70}")
    
    if total_errors == 0:
        print("✅ STRESS TEST PASSED - No errors!")
    else:
        print(f"⚠️  STRESS TEST COMPLETED with {total_errors} errors")


def main():
    host = "127.0.0.1"
    port = 9000
    num_clients = 10
    events_per_client = 1000
    
    if len(sys.argv) >= 2:
        host = sys.argv[1]
    if len(sys.argv) >= 3:
        port = int(sys.argv[2])
    if len(sys.argv) >= 4:
        num_clients = int(sys.argv[3])
    if len(sys.argv) >= 5:
        events_per_client = int(sys.argv[4])
    
    run_stress_test(host, port, num_clients, events_per_client)


if __name__ == "__main__":
    main()
