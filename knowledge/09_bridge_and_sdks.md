# 09 — C API Bridge & Polyglot SDKs

> **Goal:** Understand how a C++ engine is exposed via a C ABI, and consumed by Python (ctypes) and Go (cgo) SDKs.

---

## 1. Why a C API Bridge?

C++ has **no stable ABI** — name mangling, vtable layout, exception handling, and `std::string` representation vary between compilers and versions. Linking a Python extension or Go binary directly to C++ is fragile.

**C has a stable ABI.** Every platform defines exactly how C functions are called (calling convention, parameter passing, return values). The C bridge:

1. Hides all C++ internals behind `extern "C"` functions.
2. Uses only C-compatible types (`int`, `uint8_t*`, `const char*`, structs of PODs).
3. Compiles to `libesccore.so` — loadable by any language via FFI.

---

## 2. C API Design (`esccore.h`)

**File:** `include/eventstream/bridge/esccore.h`

### 2.1 Error Codes

```c
typedef enum {
    ESC_OK            =  0,
    ESC_ERR_INIT      = -1,   // Engine not initialized
    ESC_ERR_BACKPRES  = -2,   // Backpressure (queue full)
    ESC_ERR_INVALID   = -3,   // Invalid argument
    ESC_ERR_TIMEOUT   = -4,   // Operation timed out
    ESC_ERR_FULL      = -5,   // Queue at capacity
    ESC_ERR_INTERNAL  = -99   // Internal C++ exception
} esc_status_t;
```

**Convention:** 0 = success, negative = error. This is the universal C error convention (used by POSIX, Linux kernel, SQLite, etc.).

### 2.2 Data Structures

```c
typedef struct {
    uint32_t       id;
    esc_priority_t priority;
    const char*    topic;        // null-terminated string
    const uint8_t* body;         // raw byte pointer
    size_t         body_len;
} esc_event_t;
```

**Key rules:**
- No `std::string` → use `const char*` (null-terminated).
- No `std::vector` → use pointer + length.
- No `std::shared_ptr` → the bridge manages lifetime internally.

### 2.3 API Surface

| Function | Purpose |
|----------|---------|
| `esccore_init(config_path)` | Start the engine: load config, create all components |
| `esccore_shutdown()` | Stop the engine, flush storage, join threads |
| `esccore_push(event)` | Push one event into the Dispatcher |
| `esccore_push_batch(events, count, pushed)` | Push multiple events, report how many succeeded |
| `esccore_subscribe(topic_prefix, callback, user_data)` | Register a callback for processed events |
| `esccore_metrics(out)` | Get aggregated metrics snapshot |
| `esccore_health(out)` | Get health status (alive, ready, backpressure level) |
| `esccore_metrics_prometheus(buf, len, written)` | Render Prometheus text format into a buffer |
| `esccore_pause()` | Pause the pipeline |
| `esccore_resume()` | Resume the pipeline |

### 2.4 Callback Convention

```c
typedef int (*esc_event_callback_t)(const esc_event_t* event, void* user_data);
```

- Returns `int`: 0 = continue, non-zero = unsubscribe.
- `user_data` is an opaque pointer — the caller can pass any context (e.g., a Python object pointer).

---

## 3. Bridge Implementation (`esccore.cpp`)

**File:** `src/bridge/esccore.cpp`

### 3.1 Engine Singleton

```cpp
struct Engine {
    std::unique_ptr<EventStream::EventBusMulti> bus;
    std::unique_ptr<Dispatcher> dispatcher;
    std::unique_ptr<StorageEngine> storage;
    std::unique_ptr<ProcessManager> manager;
    std::unique_ptr<EventStream::Admin> admin;
    std::atomic<bool> initialized{false};
};

static Engine g_engine;
```

**Why a global?** The C API has no object handle — functions like `esccore_push()` operate on the implicit global instance. This is the simplest design for FFI.

### 3.2 Type Conversion

```cpp
static EventStream::Event toInternal(const esc_event_t* e) {
    EventStream::EventHeader h;
    h.id = e->id;
    h.priority = static_cast<EventStream::EventPriority>(e->priority);
    h.sourceType = EventStream::EventSourceType::PYTHON;  // FFI origin
    h.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    h.body_len = static_cast<uint32_t>(e->body_len);
    h.topic_len = static_cast<uint16_t>(std::strlen(e->topic));
    h.crc32 = 0;  // CRC computed separately if needed

    return EventStream::Event(
        h,
        std::string(e->topic),
        std::vector<uint8_t>(e->body, e->body + e->body_len),
        {}
    );
}
```

### 3.3 Exception Firewall

```cpp
esc_status_t esccore_push(const esc_event_t* event) {
    if (!g_engine.initialized.load()) return ESC_ERR_INIT;
    if (!event || !event->topic)       return ESC_ERR_INVALID;

    try {
        auto internal = toInternal(event);
        auto ptr = std::make_shared<EventStream::Event>(std::move(internal));
        if (!g_engine.dispatcher->tryPush(ptr))
            return ESC_ERR_BACKPRES;
        return ESC_OK;
    } catch (...) {
        return ESC_ERR_INTERNAL;
    }
}
```

**Critical:** Every C API function wraps its body in `try/catch`. C++ exceptions **must not** cross the `extern "C"` boundary — doing so is undefined behavior. The catch-all converts any exception to an error code.

---

## 4. Python SDK (ctypes)

**Directory:** `sdk/python/esccore/`

### 4.1 Architecture

```
Python user code
      │
      ▼
  esccore.engine.Engine   ← Python class wrapping ctypes
      │
      ▼
  ctypes.cdll.LoadLibrary("libesccore.so")
      │
      ▼
  esccore_push() (C function in libesccore.so)
      │
      ▼
  C++ Dispatcher::tryPush()
```

### 4.2 ctypes Struct Mirrors

```python
class EscEvent(ctypes.Structure):
    _fields_ = [
        ("id",       ctypes.c_uint32),
        ("priority", ctypes.c_int),
        ("topic",    ctypes.c_char_p),
        ("body",     ctypes.POINTER(ctypes.c_uint8)),
        ("body_len", ctypes.c_size_t),
    ]
```

This Python class has the **exact same memory layout** as the C `esc_event_t`. When passed to a C function, ctypes marshals it directly — no serialization, no copying.

### 4.3 Engine Class

```python
class Engine:
    def __init__(self, lib_path: str = "libesccore.so"):
        self._lib = ctypes.cdll.LoadLibrary(lib_path)
        # Set argument types for type safety:
        self._lib.esccore_push.argtypes = [ctypes.POINTER(EscEvent)]
        self._lib.esccore_push.restype  = ctypes.c_int

    def push(self, event: EventData) -> None:
        c_event = EscEvent()
        c_event.id = event.id
        c_event.priority = event.priority.value
        c_event.topic = event.topic.encode("utf-8")
        body_arr = (ctypes.c_uint8 * len(event.body))(*event.body)
        c_event.body = ctypes.cast(body_arr, ctypes.POINTER(ctypes.c_uint8))
        c_event.body_len = len(event.body)

        status = self._lib.esccore_push(ctypes.byref(c_event))
        if status != 0:
            raise RuntimeError(f"esccore_push failed: {status}")
```

### 4.4 FastAPI Adapter

```python
app = FastAPI(title="EventStreamCore Gateway")

@app.post("/events")
async def push_event(req: EventRequest):
    engine.push(EventData(id=req.id, priority=req.priority, ...))
    return {"status": "ok"}

@app.get("/metrics")
async def get_metrics():
    return engine.metrics()

@app.get("/health")
async def get_health():
    return engine.health()
```

This turns the C++ engine into a **REST API** with one line of deployment (`uvicorn esccore.adapter:app`).

---

## 5. Go SDK (cgo)

**File:** `sdk/go/esccore/esccore.go`

### 5.1 cgo + dlopen Architecture

```go
/*
#cgo LDFLAGS: -ldl
#include <dlfcn.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    int priority;
    const char* topic;
    const uint8_t* body;
    size_t body_len;
} esc_event_t;

// Function pointer types
typedef int (*esccore_init_fn)(const char*);
typedef int (*esccore_push_fn)(const esc_event_t*);
// ... etc
*/
import "C"
```

### 5.2 Dynamic Loading

```go
func New(libPath string) (*Engine, error) {
    cPath := C.CString(libPath)
    defer C.free(unsafe.Pointer(cPath))

    handle := C.dlopen(cPath, C.RTLD_NOW)
    if handle == nil {
        return nil, fmt.Errorf("dlopen failed: %s", C.GoString(C.dlerror()))
    }

    e := &Engine{handle: handle}
    e.fnInit = C.dlsym(handle, C.CString("esccore_init"))
    e.fnPush = C.dlsym(handle, C.CString("esccore_push"))
    // ... resolve all function pointers
    return e, nil
}
```

**Why dlopen instead of direct linking?**  
Direct linking (`#cgo LDFLAGS: -lesccore`) requires the library at Go build time. `dlopen` loads the library at runtime — the Go binary can be built independently and the `.so` path provided at deployment.

### 5.3 Push Function

```go
func (e *Engine) Push(id uint32, priority int, topic string, body []byte) error {
    cTopic := C.CString(topic)
    defer C.free(unsafe.Pointer(cTopic))

    var cBody *C.uint8_t
    if len(body) > 0 {
        cBody = (*C.uint8_t)(unsafe.Pointer(&body[0]))
    }

    evt := C.esc_event_t{
        id:       C.uint32_t(id),
        priority: C.int(priority),
        topic:    cTopic,
        body:     cBody,
        body_len: C.size_t(len(body)),
    }

    status := C.int(C.call_esccore_push(e.fnPush, &evt))
    if status != 0 {
        return fmt.Errorf("esccore_push failed: %d", status)
    }
    return nil
}
```

---

## 6. FFI Design Principles

| Principle | How Applied |
|-----------|------------|
| **Stable ABI** | `extern "C"` + C-only types |
| **Exception firewall** | Every function has `try/catch(...)` |
| **Null safety** | All pointer parameters checked before use |
| **Opaque types** | No C++ types in the public header |
| **Error codes** | Integer return values, no exceptions |
| **Zero-copy where possible** | `const uint8_t*` body pointer, not a copy |
| **Thread safety** | Global engine with atomic `initialized` flag |
| **Versioning** | `SO_VERSION` and `SOVERSION` in CMake |

---

## 7. Build & Link

### CMake

```cmake
add_library(esccore SHARED esccore.cpp)
target_link_libraries(esccore PUBLIC eventstream_core spdlog::spdlog yaml-cpp)
set_target_properties(esccore PROPERTIES
    OUTPUT_NAME "esccore"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    C_VISIBILITY_PRESET default      # export C symbols
    CXX_VISIBILITY_PRESET default)
```

### Python usage

```bash
pip install -e sdk/python/
export LD_LIBRARY_PATH=/path/to/build:$LD_LIBRARY_PATH
python -c "from esccore import Engine; e = Engine(); e.init('config/config.yaml')"
```

### Go usage

```bash
cd sdk/go
go build ./...
LD_LIBRARY_PATH=/path/to/build go test ./...
```

---

## 8. Interview Questions

**Q1: Why not use gRPC/Protobuf instead of a C API?**  
A: gRPC adds serialization overhead, network latency (even over localhost), and requires code generation. A C API via `dlopen` is **zero-copy, in-process, sub-microsecond** — orders of magnitude faster.

**Q2: What happens if a Python callback raises an exception?**  
A: In the ctypes callback, Python exceptions don't propagate to C. The callback function in C would receive whatever the Python callback returns (potentially garbage). The SDK should catch exceptions inside the callback wrapper.

**Q3: Why does the Go SDK use `unsafe.Pointer`?**  
A: cgo requires raw pointers to pass data to C. `unsafe.Pointer` is Go's way of saying "I'm responsible for memory safety here." It's necessary for FFI but should be isolated behind a safe Go API.

**Q4: How would you version the C API?**  
A: 1. **Symbol versioning** (Linux `ld` `--version-script`) — allows multiple ABI versions in one `.so`. 2. **SOVERSION** in CMake — consumers link against `libesccore.so.1` (major version). 3. **API version function** — `int esccore_version()` returns a compile-time constant.

**Q5: What is the performance overhead of the C bridge?**  
A: Per-call overhead is ~50-100ns (function call + `make_shared` + `toInternal` copy). For events with small payloads (< 1KB), this is dominated by the `vector<uint8_t>` copy. For zero-copy, the C API could accept an opaque handle that the bridge maps to an internal event — avoiding the copy entirely.

---

## 9. Deep Dive: Why `extern "C"` and Name Mangling

### 9.1 C++ Name Mangling

C++ compilers encode function signatures into symbol names to support overloading:

```cpp
void push(int x);          // Mangled: _Z4pushi
void push(double x);       // Mangled: _Z4pushd
void push(int x, int y);   // Mangled: _Z4pushii
```

Each compiler has its own mangling scheme. GCC uses the Itanium ABI (`_Z` prefix), MSVC uses a completely different scheme (`?push@@YAXH@Z`). This means a library compiled with GCC cannot be loaded by code compiled with MSVC (or even a different GCC version in some cases).

### 9.2 `extern "C"` Disables Mangling

```cpp
extern "C" int esccore_push(const esc_event_t* event);
// Symbol: esccore_push  (no mangling, stable across compilers)
```

The symbol is now a plain C name. Every language's FFI can find it by name. This is why the C API bridge exists — it's a **name stability layer** between the C++ internals and the outside world.

### 9.3 Verifying Symbols

```bash
nm -D libesccore.so | grep esccore
# T esccore_init         ← T = text (code) section, exported
# T esccore_push
# T esccore_shutdown
# T esccore_set_callback
```

If you forgot `extern "C"`, you'd see mangled names like `_Z12esccore_pushPK11esc_event_t` — no Python ctypes or Go cgo could find that.

---

## 10. Deep Dive: `dlopen`/`dlsym` Internals

### 10.1 Dynamic Linking Flow

```
Python: ctypes.cdll.LoadLibrary("libesccore.so")
  └→ dlopen("libesccore.so", RTLD_LAZY)
       └→ Kernel: mmap() the .so into the process address space
            └→ ELF loader: parse .dynamic section
                 └→ Resolve NEEDED dependencies (libstdc++, libc, libpthread, spdlog, yaml-cpp)
                      └→ Recursively dlopen each dependency
       └→ Return opaque handle

Python: lib.esccore_push
  └→ dlsym(handle, "esccore_push")
       └→ Search .dynsym (dynamic symbol table) by hash
            └→ Return function pointer: 0x7f1234567890
```

### 10.2 RTLD_LAZY vs RTLD_NOW

```c
dlopen("libesccore.so", RTLD_LAZY);  // Resolve symbols on first use
dlopen("libesccore.so", RTLD_NOW);   // Resolve ALL symbols at load time
```

`RTLD_LAZY` is faster to load but may crash at runtime if a symbol is missing. `RTLD_NOW` is slower but fails immediately — preferable for production.

### 10.3 Symbol Resolution Order

When multiple `.so` files define the same symbol, the linker resolves using **breadth-first search** of the dependency tree:

```
1. Main executable
2. Libraries in LD_PRELOAD
3. Direct dependencies (in order listed in ELF NEEDED)
4. Transitive dependencies (BFS)
```

This is why `LD_PRELOAD` can override any function — it's searched first.

---

## 11. Deep Dive: ABI Stability Concerns

### 11.1 What Breaks ABI

| Change | ABI Break? | Why |
|--------|-----------|-----|
| Add new function | ✅ Safe | New symbol, old ones unchanged |
| Remove function | ❌ Breaks | Existing callers can't find symbol |
| Change parameter type | ❌ Breaks | Callers pass wrong-sized data |
| Add field to struct | ❌ Breaks | Callers have wrong `sizeof`, offsets shift |
| Reorder struct fields | ❌ Breaks | Callers read wrong offsets |
| Change enum values | ❌ Breaks | Callers pass wrong integers |
| Change return type | ❌ Breaks | Callers interpret return value incorrectly |

### 11.2 Project's ABI Strategy

The `esc_event_t` struct is the critical ABI surface:

```c
typedef struct {
    const char*    topic;
    int            priority;
    const uint8_t* body;
    size_t         body_len;
} esc_event_t;
```

**Safe evolution rules:**
1. Never reorder existing fields.
2. Never change field types.
3. To add fields, create `esc_event_v2_t` with a `version` field.
4. Or use **opaque handles**: `typedef struct esc_event_opaque* esc_event_handle_t;` — internal layout is hidden, so changes don't break callers.

### 11.3 Symbol Versioning (Advanced)

```
# version.map
ESCCORE_1.0 {
    global: esccore_init; esccore_push; esccore_shutdown;
    local: *;
};
ESCCORE_2.0 {
    global: esccore_push_v2;  # New function
} ESCCORE_1.0;
```

```cmake
target_link_options(esccore PRIVATE -Wl,--version-script=${CMAKE_SOURCE_DIR}/version.map)
```

This allows **multiple versions of the same function** in one library. Old Python code calls `esccore_push@ESCCORE_1.0`; new code calls `esccore_push_v2@ESCCORE_2.0`.

---

## 12. Deep Dive: Undefined Behavior at FFI Boundaries

### 12.1 UB Catalog

| Scenario | UB Type | Project Mitigation |
|----------|---------|-------------------|
| C++ exception crosses `extern "C"` | UB (stack unwinding stops) | `try/catch(...)` in every bridge function |
| Null pointer dereference | UB | Null checks on all pointer parameters |
| Use-after-free (caller frees buffer, bridge still reads) | UB | Bridge copies body into `vector<uint8_t>` immediately |
| Data race on global engine pointer | UB | `std::atomic<bool> initialized` guards access |
| Misaligned pointer | UB (on some architectures) | C struct uses natural alignment |
| Buffer overflow (body_len > actual buffer) | UB | Bridge trusts caller — production would add bounds checking |
| Type punning via `void*` | UB if aliasing rules violated | Bridge casts via `reinterpret_cast` only for opaque handles |

### 12.2 Exception Firewall Pattern

```cpp
extern "C" int esccore_push(const esc_event_t* event) {
    try {
        // All C++ code here...
        return 0;  // success
    } catch (const std::exception& e) {
        spdlog::error("Bridge exception: {}", e.what());
        return -1;
    } catch (...) {
        spdlog::error("Bridge: unknown exception");
        return -2;
    }
}
```

The `catch(...)` is critical — it catches non-`std::exception` types like `int` throws or structured exceptions. Without it, the exception would escape `extern "C"` and cause UB (typically a crash with no backtrace).

---

## 13. Deep Dive: Python ctypes Safety Pitfalls

### 13.1 Lifetime Management

```python
# DANGEROUS — Python may garbage-collect `body` while C is still reading it
body = b"hello"
event = esc_event_t(topic=b"test", body=body, body_len=len(body))
lib.esccore_push(ctypes.byref(event))
# If body was the last reference, GC could collect it before push() returns
```

**Fix:** The bridge immediately copies `body` into a `std::vector<uint8_t>`, so it doesn't matter when Python GCs the original buffer.

### 13.2 Callback Pitfalls

```python
# DANGEROUS — callback goes out of scope, GC collects the function object
def setup():
    def my_callback(event):
        print(event)
    cb = CALLBACK_TYPE(my_callback)
    lib.esccore_set_callback(cb)
    # cb goes out of scope when setup() returns!
    # Next callback invocation → segfault (calling freed memory)
```

**Fix:** Store the callback reference at module level:

```python
_global_callback_ref = None  # prevent GC

def set_callback(fn):
    global _global_callback_ref
    _global_callback_ref = CALLBACK_TYPE(fn)
    lib.esccore_set_callback(_global_callback_ref)
```

### 13.3 GIL (Global Interpreter Lock) Impact

When Python calls a ctypes function, the GIL is **released** during the C call. This means:
- Other Python threads can run while C code executes — good for parallelism.
- But the C callback will be called from a C++ thread, which doesn't hold the GIL.
- Calling Python objects from the callback without acquiring the GIL → crash.

**Fix for callback:** Use `PyGILState_Ensure()` / `PyGILState_Release()` in a C wrapper, or ensure the ctypes callback type handles this automatically (ctypes does acquire the GIL for callbacks).

---

## 14. Deep Dive: Go cgo Overhead

### 14.1 cgo Call Cost

```
Go function call:          ~2 ns
cgo C function call:       ~50-100 ns (25-50× slower)
```

Why so expensive? Each cgo call:
1. **Saves Go registers** onto the goroutine stack.
2. **Switches to a system thread stack** (Go goroutines use tiny stacks, C expects 8MB).
3. **Pins the goroutine** to the current OS thread (`LockOSThread`).
4. Executes the C function.
5. **Unpins**, restores Go state.

### 14.2 Goroutine Scheduling Impact

Go's scheduler (M:N model) multiplexes thousands of goroutines onto a small number of OS threads. A cgo call **blocks the OS thread**, reducing available threads for other goroutines.

```
Before cgo call:
  Thread 1: goroutine A  goroutine B  goroutine C  (switching rapidly)
  Thread 2: goroutine D  goroutine E

During cgo call from goroutine A:
  Thread 1: goroutine A [BLOCKED IN C]  ← locked to this thread
  Thread 2: goroutine B  goroutine C  goroutine D  goroutine E  (overloaded!)
  Thread 3: [may be spawned by runtime to compensate]
```

If many goroutines call cgo simultaneously, the runtime spawns extra OS threads. With `GOMAXPROCS=4` and 100 concurrent cgo calls, you'd have 104 threads — losing Go's lightweight concurrency advantage.

### 14.3 Mitigation: Batch API

Instead of calling `esccore_push()` per event:

```go
// Batch: one cgo call for N events
func (e *Engine) PushBatch(events []Event) error {
    cEvents := make([]C.esc_event_t, len(events))
    // ... fill cEvents ...
    C.esccore_push_batch(&cEvents[0], C.size_t(len(events)))
    return nil
}
```

Amortize the 100ns cgo overhead across N events. For N=100, overhead drops from 100ns/event to 1ns/event.

---

## 15. Extended Interview Questions

**Q6: What is the One Definition Rule (ODR) and how does it relate to shared libraries?**  
A: ODR says each entity must have exactly one definition in the entire program. In shared libraries, if two `.so` files define the same inline function differently (e.g., different versions of a header), you get ODR violation → UB. The project mitigates this by using `CXX_VISIBILITY_PRESET hidden` and only exporting `extern "C"` symbols.

**Q7: What is `__attribute__((visibility("default")))` and why does it matter?**  
A: GCC/Clang default to exporting all symbols. With `-fvisibility=hidden`, all symbols are hidden unless marked with `__attribute__((visibility("default")))`. This reduces the `.dynsym` table size (faster `dlsym`), avoids symbol conflicts, and reduces the attack surface. `extern "C"` functions in the bridge get default visibility; everything else is hidden.

**Q8: What happens if you call `esccore_push()` from two Go goroutines simultaneously?**  
A: Both cgo calls enter the C bridge on different OS threads. The bridge's `toInternal()` creates independent copies. The `mpsc_push()` is lock-free and thread-safe. So it's safe — no locking needed. But both goroutines' OS threads are blocked during the cgo call, reducing parallelism for other goroutines.

**Q9: How would you test the C API for memory leaks?**  
A: Use Valgrind or AddressSanitizer. Build with `-fsanitize=address`, then run a test that initializes the engine, pushes 10K events, and shuts down. ASan reports any leaked allocations. For the Python SDK, also check that ctypes callback references aren't GC'd prematurely.

**Q10: Why not use Protocol Buffers for the SDK interface instead of raw structs?**  
A: Protobuf adds serialization overhead (~200-500ns per event) and a build-time dependency (protoc code generation). The C struct approach is zero-serialization (just pointer passing), which matters when the SDK is in the same process. For an out-of-process SDK (e.g., gRPC), Protobuf would be the right choice.
