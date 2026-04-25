// Package esccore wraps libesccore.so via cgo, providing a Go-native
// interface to the EventStreamCore engine.
//
// Events flow from the C++ pipeline OUT to Go through callbacks registered
// with Engine.Subscribe.  There is no push path from Go into the engine.
//
// The shared library must be built first:
//
//cd EventStreamCore/build && cmake .. && make esccore
package esccore

/*
#cgo LDFLAGS: -ldl
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>

// ── Mirror esccore.h types ──────────────────────────────────────────────────

typedef enum {
    ESC_OK            =  0,
    ESC_ERR_INIT      = -1,
    ESC_ERR_INVALID   = -3,
    ESC_ERR_INTERNAL  = -99
} esc_status_t;

typedef enum {
    ESC_PRIORITY_BATCH    = 0,
    ESC_PRIORITY_LOW      = 1,
    ESC_PRIORITY_MEDIUM   = 2,
    ESC_PRIORITY_HIGH     = 3,
    ESC_PRIORITY_CRITICAL = 4
} esc_priority_t;

typedef struct {
    uint32_t       id;
    esc_priority_t priority;
    const char*    topic;
    const uint8_t* body;
    size_t         body_len;
} esc_event_t;

typedef struct {
    uint64_t total_events_processed;
    uint64_t total_events_dropped;
    uint64_t queue_depth;
    int      backpressure_level;
} esc_metrics_t;

typedef struct {
    int is_alive;
    int is_ready;
    int backpressure_level;
} esc_health_t;

typedef int (*esc_event_callback_t)(const esc_event_t* event, void* user_data);

// ── dlopen wrapper ──────────────────────────────────────────────────────────

static void* lib_handle = NULL;

typedef esc_status_t (*fn_init)(const char*);
typedef esc_status_t (*fn_shutdown)(void);
typedef esc_status_t (*fn_subscribe)(const char*, esc_event_callback_t, void*);
typedef esc_status_t (*fn_metrics)(esc_metrics_t*);
typedef esc_status_t (*fn_health)(esc_health_t*);

static fn_init      p_init;
static fn_shutdown  p_shutdown;
static fn_subscribe p_subscribe;
static fn_metrics   p_metrics;
static fn_health    p_health;

static int load_lib(const char* path) {
    lib_handle = dlopen(path, RTLD_NOW);
    if (!lib_handle) return -1;

    p_init      = (fn_init)     dlsym(lib_handle, "esccore_init");
    p_shutdown  = (fn_shutdown) dlsym(lib_handle, "esccore_shutdown");
    p_subscribe = (fn_subscribe)dlsym(lib_handle, "esccore_subscribe");
    p_metrics   = (fn_metrics)  dlsym(lib_handle, "esccore_metrics");
    p_health    = (fn_health)   dlsym(lib_handle, "esccore_health");

    if (!p_init || !p_shutdown || !p_subscribe || !p_metrics || !p_health)
        return -2;
    return 0;
}

static int call_init(const char* cfg)               { return p_init(cfg); }
static int call_shutdown(void)                       { return p_shutdown(); }
static int call_subscribe(const char* pfx,
                          esc_event_callback_t cb,
                          void* ud)                 { return p_subscribe(pfx, cb, ud); }
static int call_metrics(esc_metrics_t* m)            { return p_metrics(m); }
static int call_health(esc_health_t* h)              { return p_health(h); }
*/
import "C"

import (
"fmt"
"unsafe"
)

// Priority mirrors esc_priority_t.
type Priority int

const (
PriorityBATCH    Priority = 0
PriorityLOW      Priority = 1
PriorityMEDIUM   Priority = 2
PriorityHIGH     Priority = 3
PriorityCRITICAL Priority = 4
)

// Event is the Go-side event descriptor, filled inside a callback.
type Event struct {
ID       uint32
Topic    string
Body     []byte
Priority Priority
}

// Metrics mirrors esc_metrics_t.
type Metrics struct {
TotalEventsProcessed uint64
TotalEventsDropped   uint64
QueueDepth           uint64
BackpressureLevel    int
}

// Health mirrors esc_health_t.
type Health struct {
IsAlive           bool
IsReady           bool
BackpressureLevel int
}

// EventCallback is called for each processed event delivered by the engine.
// Return false to unsubscribe.
type EventCallback func(evt Event) bool

// Engine wraps a loaded libesccore instance.
type Engine struct {
loaded bool
}

// New loads libesccore.so from the given path.
func New(libPath string) (*Engine, error) {
cPath := C.CString(libPath)
defer C.free(unsafe.Pointer(cPath))

rc := C.load_lib(cPath)
if rc != 0 {
return nil, fmt.Errorf("esccore: failed to load %s (code %d)", libPath, rc)
}
return &Engine{loaded: true}, nil
}

func check(rc C.int) error {
if rc == 0 {
return nil
}
return fmt.Errorf("esccore error %d", int(rc))
}

// Init starts the engine with the given YAML config.
func (e *Engine) Init(configPath string) error {
c := C.CString(configPath)
defer C.free(unsafe.Pointer(c))
return check(C.call_init(c))
}

// Shutdown stops the engine gracefully.
func (e *Engine) Shutdown() error {
return check(C.call_shutdown())
}

// Subscribe registers fn to receive every processed event whose topic starts
// with topicPrefix (pass "" for all topics).
//
// fn is called from an internal C++ thread — keep it non-blocking.
// Return false from fn to unsubscribe.
func (e *Engine) Subscribe(topicPrefix string, fn EventCallback) error {
// We wrap the Go callback in a C-callable trampoline stored in the global
// registry so the GC cannot collect it.
idx := registerCallback(fn)

var cPrefix *C.char
if topicPrefix != "" {
cPrefix = C.CString(topicPrefix)
defer C.free(unsafe.Pointer(cPrefix))
}

return check(C.call_subscribe(cPrefix, C.esc_event_callback_t(C.goCallbackTrampoline), unsafe.Pointer(uintptr(idx))))
}

// Metrics returns the current engine metrics.
func (e *Engine) Metrics() (Metrics, error) {
var cm C.esc_metrics_t
if err := check(C.call_metrics(&cm)); err != nil {
return Metrics{}, err
}
return Metrics{
TotalEventsProcessed: uint64(cm.total_events_processed),
TotalEventsDropped:   uint64(cm.total_events_dropped),
QueueDepth:           uint64(cm.queue_depth),
BackpressureLevel:    int(cm.backpressure_level),
}, nil
}

// Health returns the current engine health status.
func (e *Engine) Health() (Health, error) {
var ch C.esc_health_t
if err := check(C.call_health(&ch)); err != nil {
return Health{}, err
}
return Health{
IsAlive:           ch.is_alive != 0,
IsReady:           ch.is_ready != 0,
BackpressureLevel: int(ch.backpressure_level),
}, nil
}
