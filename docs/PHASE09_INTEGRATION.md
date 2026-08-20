# Phase 9 — Integration: Config, C API, SDK, End-to-End Demo

## Mục tiêu

Tích hợp tất cả modules vào EventStreamCore, mở rộng C API, Go/Python SDK, viết end-to-end demo.

---

## 1. Configuration Extension

### JSON/YAML config

```json
{
  "ingest": {
    "tcp": {"port": 9000, "io_uring": true},
    "posix_mq": {"name": "/eventstream"},
    "socketcan": {"interface": "vcan0"}
  },
  "dispatch": {
    "type": "work_stealing",
    "workers": 8
  },
  "rt": {
    "policy": "SCHED_FIFO",
    "priority": 80,
    "cpu_affinity": [2, 3]
  }
}
```

### Config loader

```cpp
struct RuntimeConfig {
    bool ioUringEnabled = false;
    bool posixMqEnabled = false;
    bool socketCanEnabled = false;
    std::string canInterface = "vcan0";
    RtConfig rt;
    DispatchConfig dispatch;
};

RuntimeConfig loadConfig(const std::string& path);
```

---

## 2. C API Extension

```c
// include/eventstream.h

typedef struct esc_ctx esc_ctx_t;

esc_ctx_t* esc_create(const esc_config_t* config);
void esc_destroy(esc_ctx_t* ctx);
int esc_publish(esc_ctx_t* ctx, const char* topic, const void* data, size_t len);
int esc_subscribe(esc_ctx_t* ctx, const char* topic, esc_callback_t cb, void* user);
int esc_poll(esc_ctx_t* ctx, int timeout_ms);
```

### Implementation

```cpp
extern "C" esc_ctx_t* esc_create(const esc_config_t* config) {
    auto* ctx = new esc_ctx_t{};
    ctx->engine = std::make_unique<EventStreamEngine>(convert(*config));
    ctx->engine->start();
    return ctx;
}
```

---

## 3. Go SDK

```go
package eventstream

// #cgo LDFLAGS: -leventstream
// #include <eventstream.h>
import "C"

type Client struct {
    ctx *C.esc_ctx_t
}

func New(cfg Config) (*Client, error) {
    c := C.esc_create(&cCfg)
    return &Client{ctx: c}, nil
}

func (c *Client) Publish(topic string, data []byte) error {
    ctopic := C.CString(topic)
    defer C.free(unsafe.Pointer(ctopic))
    rc := C.esc_publish(c.ctx, ctopic, unsafe.Pointer(&data[0]), C.size_t(len(data)))
    if rc != 0 { return fmt.Errorf("publish failed: %d", rc) }
    return nil
}
```

---

## 4. Python SDK

```python
import ctypes
from ctypes import c_void_p, c_size_t, c_char_p, c_int

lib = ctypes.CDLL("./libeventstream.so")

class Client:
    def __init__(self, config):
        self.ctx = lib.esc_create(config)

    def publish(self, topic: str, data: bytes):
        return lib.esc_publish(self.ctx, topic.encode(), data, len(data))
```

---

## 5. End-to-End Demo

### Scenario

1. Producer gửi CAN frame qua `vcan0`.
2. `SocketCanIngestServer` nhận frame.
3. `WorkStealingDispatcher` phân phối event.
4. `RealtimeProcessor` xử lý với SCHED_FIFO.
5. Kết quả ghi ra POSIX message queue.
6. Python client nhận từ MQ.

### Script

```bash
# Terminal 1: start engine
./eventstream --config demo.json

# Terminal 2: send CAN frame
cansend vcan0 123#DEADBEEF

# Terminal 3: Python consumer
python3 demo_consumer.py
```

---

## 6. Metrics & Observability

```cpp
struct Metrics {
    std::atomic<uint64_t> eventsIngested{0};
    std::atomic<uint64_t> eventsProcessed{0};
    std::atomic<uint64_t> eventsDropped{0};
    std::atomic<uint64_t> latencyUs{0};
};

// Expose qua HTTP /metrics hoặc POSIX SHM
```

---

## 7. Interview Q&A

**Q: Tại sao cần C API?**
A: C API là lingua franca, cho phép Go/Python/Rust gọi vào engine C++.

**Q: End-to-end demo quan trọng thế nào?**
A: Chứng minh system hoạt động thực tế, không chỉ là unit test.

**Q: Metrics nên expose ở đâu?**
A: POSIX SHM cho low-latency, HTTP /metrics cho dễ integrate.

---

## 8. References

- cgo documentation
- ctypes documentation
- nlohmann/json hoặc yaml-cpp
