#!/usr/bin/env python3
"""
Simple TCP Event Sender for manual testing
Usage: python3 send_tcp_event.py [host] [port] [num_events] [topic]
"""

import socket
import struct
import time
import json
import sys

def create_event_frame(event_id: int, topic: str = "test.manual", payload_size: int = 256, priority: int = 2) -> bytes:
    """
    Create event frame for EventStreamCore
    
    Frame format:
    [4-byte frame_length (big-endian)]
    [1-byte priority: 0=BATCH, 1=LOW, 2=MEDIUM, 3=HIGH, 4=CRITICAL]
    [2-byte topic_len (big-endian)]
    [topic string]
    [payload bytes]
    """
    topic_bytes = topic.encode('utf-8')
    topic_len = len(topic_bytes)
    
    # Create JSON payload
    payload = json.dumps({
        "event_id": event_id,
        "timestamp": int(time.time() * 1000),
        "data": f"Test event #{event_id}",
        "padding": "x" * max(0, payload_size - 100)
    })
    
    if len(payload) < payload_size:
        payload += " " * (payload_size - len(payload))
    payload_bytes = payload[:payload_size].encode('utf-8')
    
    # Build frame body
    frame_body = struct.pack('B', priority)           # 1 byte priority
    frame_body += struct.pack('>H', topic_len)        # 2 bytes topic length (big-endian)
    frame_body += topic_bytes                          # topic string
    frame_body += payload_bytes                        # payload
    
    # Frame length header (big-endian 4 bytes)
    frame_len = len(frame_body)
    length_header = struct.pack('>I', frame_len)
    
    return length_header + frame_body


def send_tcp_events(host: str, port: int, num_events: int, topic: str):
    """Send TCP events to EventStreamCore"""
    print(f"╔{'═'*60}╗")
    print(f"║  TCP Event Sender                                          ║")
    print(f"╠{'═'*60}╣")
    print(f"║  Host: {host}:{port}")
    print(f"║  Events: {num_events}")
    print(f"║  Topic: {topic}")
    print(f"╚{'═'*60}╝\n")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect((host, port))
        print(f"✅ Connected to {host}:{port}")
        
        start_time = time.time()
        
        for i in range(num_events):
            # Rotate priority: 0-4
            priority = i % 5
            priority_names = ["BATCH", "LOW", "MEDIUM", "HIGH", "CRITICAL"]
            
            frame = create_event_frame(i, topic, 256, priority)
            sock.sendall(frame)
            
            print(f"  📤 Sent event #{i} | priority={priority_names[priority]} | topic={topic}")
            
            # Small delay for visibility
            if num_events <= 10:
                time.sleep(0.1)
        
        elapsed = time.time() - start_time
        throughput = num_events / elapsed if elapsed > 0 else 0
        
        print(f"\n{'='*60}")
        print(f"✅ Sent {num_events} events in {elapsed:.2f}s")
        print(f"📊 Throughput: {throughput:.0f} events/sec")
        print(f"{'='*60}")
        
        sock.close()
        
    except ConnectionRefusedError:
        print(f"❌ Connection refused! Is EventStreamCore running on {host}:{port}?")
        sys.exit(1)
    except Exception as e:
        print(f"❌ Error: {e}")
        sys.exit(1)


def main():
    # Default values
    host = "127.0.0.1"
    port = 9000
    num_events = 5
    topic = "test.manual"
    
    # Parse command line args
    if len(sys.argv) >= 2:
        host = sys.argv[1]
    if len(sys.argv) >= 3:
        port = int(sys.argv[2])
    if len(sys.argv) >= 4:
        num_events = int(sys.argv[3])
    if len(sys.argv) >= 5:
        topic = sys.argv[4]
    
    send_tcp_events(host, port, num_events, topic)


if __name__ == "__main__":
    main()
