# EventStreamCore Test Suite

## Quick Start

### 1. Build & Run EventStreamCore

```bash
cd /home/worker/EventStreamCore/build_test
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./EventStreamCore ../config/config.yaml
```

### 2. Run Tests (in another terminal)

## Test Scripts

### 📤 Send TCP Events (Manual Test)
```bash
cd tests/

# Send 5 events (default)
python3 send_tcp_event.py

# Custom: python3 send_tcp_event.py [host] [port] [num_events] [topic]
python3 send_tcp_event.py 127.0.0.1 9000 10 order.created
python3 send_tcp_event.py 127.0.0.1 9000 100 payment.processed
```

### 📤 Send UDP Events (Manual Test)
```bash
# Send 5 UDP events (default)
python3 send_udp_event.py

# Custom: python3 send_udp_event.py [host] [port] [num_events] [topic]
python3 send_udp_event.py 127.0.0.1 9001 10 sensor.temperature
python3 send_udp_event.py 127.0.0.1 9001 50 metrics.cpu
```

### 🔥 Stress Test (High Load)
```bash
# Default: 10 clients x 1000 events = 10,000 events
python3 stress_test.py

# Custom: python3 stress_test.py [host] [port] [clients] [events_per_client]
python3 stress_test.py 127.0.0.1 9000 20 5000  # 100,000 events
python3 stress_test.py 127.0.0.1 9000 50 10000 # 500,000 events
```

### 🧪 Full System Test
```bash
python3 test_system.py
```

## Event Frame Format

```
┌─────────────────────────────────────────────────────────────┐
│ Frame Header (4 bytes, big-endian)                          │
│ └─ frame_length: uint32 (length of body, not including this)│
├─────────────────────────────────────────────────────────────┤
│ Frame Body                                                  │
│ ├─ priority: uint8 (1 byte)                                │
│ │   0=BATCH, 1=LOW, 2=MEDIUM, 3=HIGH, 4=CRITICAL           │
│ ├─ topic_len: uint16 (2 bytes, big-endian)                 │
│ ├─ topic: string (topic_len bytes)                         │
│ └─ payload: bytes (remaining)                              │
└─────────────────────────────────────────────────────────────┘
```

## Example: Manually Build Frame (Python)

```python
import struct

def create_frame(priority: int, topic: str, payload: bytes) -> bytes:
    topic_bytes = topic.encode('utf-8')
    
    # Build body
    body = struct.pack('B', priority)          # 1 byte priority
    body += struct.pack('>H', len(topic_bytes)) # 2 bytes topic len (big-endian)
    body += topic_bytes                         # topic string
    body += payload                             # payload bytes
    
    # Add length header
    return struct.pack('>I', len(body)) + body

# Example usage
frame = create_frame(
    priority=3,  # HIGH
    topic="order.created",
    payload=b'{"order_id": 12345}'
)
```

## Expected Server Output

When sending events, you should see logs like:
```
[INFO] Received frame: 280 bytes from 127.0.0.1 topic='test.manual' eventID=0
[INFO] Received frame: 280 bytes from 127.0.0.1 topic='test.manual' eventID=1
...
```

## Troubleshooting

### Connection Refused
- Make sure EventStreamCore is running
- Check the port in config.yaml matches your test

### Events Not Processing
- Check logs for errors
- Verify frame format is correct
- Check priority routing (HIGH/CRITICAL → REALTIME, MEDIUM/LOW → TRANSACTIONAL, BATCH → BATCH)

## Port Configuration

Default ports in `config/config.yaml`:
- **TCP**: 9000
- **UDP**: 9001
