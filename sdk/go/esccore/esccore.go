// Package esccore wraps libesccore.so via cgo, providing a Go-native
// interface to the EventStreamCore engine.
//
// The shared library must be built first:
//
//	cd EventStreamCore/build && cmake .. && make esccore
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
    ESC_ERR_BACKPRES  = -2,
    ESC_ERR_INVALID   = -3,
    ESC_ERR_TIMEOUT   = -4,
    ESC_ERR_FULL      = -5,
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
    uint64_t total_events_pushed;
    uint64_t total_events_processed;
    uint64_t total_events_dropped;
    uint64_t realtime_queue_depth;
    uint64_t transactional_queue_depth;
    uint64_t batch_queue_depth;
    uint64_t dlq_depth;
    int      backpressure_level;
} esc_metrics_t;

typedef struct {
    int is_alive;
    int is_ready;
    int backpressure_level;
} esc_health_t;

// ── dlopen wrapper functions ────────────────────────────────────────────────

static void* lib_handle = NULL;

// Function pointers
typedef esc_status_t (*fn_init)(const char*);
typedef esc_status_t (*fn_shutdown)(void);
typedef esc_status_t (*fn_push)(const esc_event_t*);
typedef esc_status_t (*fn_push_batch)(const esc_event_t*, size_t, size_t*);
typedef esc_status_t (*fn_metrics)(esc_metrics_t*);
typedef esc_status_t (*fn_health)(esc_health_t*);
typedef esc_status_t (*fn_prom)(char*, size_t, size_t*);
typedef esc_status_t (*fn_void)(void);

static fn_init       p_init;
static fn_shutdown   p_shutdown;
static fn_push       p_push;
static fn_push_batch p_push_batch;
static fn_metrics    p_metrics;
static fn_health     p_health;
static fn_prom       p_prom;
static fn_void       p_pause;
static fn_void       p_resume;

static int load_lib(const char* path) {
    lib_handle = dlopen(path, RTLD_NOW);
    if (!lib_handle) return -1;

    p_init       = (fn_init)      dlsym(lib_handle, "esccore_init");
    p_shutdown   = (fn_shutdown)  dlsym(lib_handle, "esccore_shutdown");
    p_push       = (fn_push)      dlsym(lib_handle, "esccore_push");
    p_push_batch = (fn_push_batch)dlsym(lib_handle, "esccore_push_batch");
    p_metrics    = (fn_metrics)   dlsym(lib_handle, "esccore_metrics");
    p_health     = (fn_health)    dlsym(lib_handle, "esccore_health");
    p_prom       = (fn_prom)      dlsym(lib_handle, "esccore_metrics_prometheus");
    p_pause      = (fn_void)      dlsym(lib_handle, "esccore_pause");
    p_resume     = (fn_void)      dlsym(lib_handle, "esccore_resume");

    if (!p_init || !p_shutdown || !p_push || !p_metrics || !p_health) return -2;
    return 0;
}

static int call_init(const char* cfg)         { return p_init(cfg); }
static int call_shutdown()                    { return p_shutdown(); }
static int call_push(const esc_event_t* e)    { return p_push(e); }
static int call_metrics(esc_metrics_t* m)     { return p_metrics(m); }
static int call_health(esc_health_t* h)       { return p_health(h); }
static int call_pause()                       { return p_pause(); }
static int call_resume()                      { return p_resume(); }
static int call_prom(char* b, size_t n, size_t* w) { return p_prom(b, n, w); }
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

// Event is the Go-side event descriptor.
type Event struct {
	ID       uint32
	Topic    string
	Body     []byte
	Priority Priority
}

// Metrics mirrors esc_metrics_t.
type Metrics struct {
	TotalEventsPushed       uint64
	TotalEventsProcessed    uint64
	TotalEventsDropped      uint64
	RealtimeQueueDepth      uint64
	TransactionalQueueDepth uint64
	BatchQueueDepth         uint64
	DLQDepth                uint64
	BackpressureLevel       int
}

// Health mirrors esc_health_t.
type Health struct {
	IsAlive           bool
	IsReady           bool
	BackpressureLevel int
}

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

// Push sends a single event into the engine.
func (e *Engine) Push(evt Event) error {
	cTopic := C.CString(evt.Topic)
	defer C.free(unsafe.Pointer(cTopic))

	var cEvt C.esc_event_t
	cEvt.id = C.uint32_t(evt.ID)
	cEvt.priority = C.esc_priority_t(evt.Priority)
	cEvt.topic = cTopic
	if len(evt.Body) > 0 {
		cEvt.body = (*C.uint8_t)(unsafe.Pointer(&evt.Body[0]))
		cEvt.body_len = C.size_t(len(evt.Body))
	}
	return check(C.call_push(&cEvt))
}

// Metrics returns the current engine metrics.
func (e *Engine) Metrics() (Metrics, error) {
	var cm C.esc_metrics_t
	if err := check(C.call_metrics(&cm)); err != nil {
		return Metrics{}, err
	}
	return Metrics{
		TotalEventsPushed:       uint64(cm.total_events_pushed),
		TotalEventsProcessed:    uint64(cm.total_events_processed),
		TotalEventsDropped:      uint64(cm.total_events_dropped),
		RealtimeQueueDepth:      uint64(cm.realtime_queue_depth),
		TransactionalQueueDepth: uint64(cm.transactional_queue_depth),
		BatchQueueDepth:         uint64(cm.batch_queue_depth),
		DLQDepth:                uint64(cm.dlq_depth),
		BackpressureLevel:       int(cm.backpressure_level),
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

// MetricsPrometheus returns a Prometheus text exposition string.
func (e *Engine) MetricsPrometheus() (string, error) {
	buf := make([]byte, 4096)
	var written C.size_t
	rc := C.call_prom((*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)), &written)
	if err := check(rc); err != nil {
		return "", err
	}
	return string(buf[:written]), nil
}

// Pause pauses the pipeline.
func (e *Engine) Pause() error { return check(C.call_pause()) }

// Resume resumes the pipeline.
func (e *Engine) Resume() error { return check(C.call_resume()) }
