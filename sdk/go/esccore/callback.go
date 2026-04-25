package esccore

/*
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint32_t       id;
    int            priority;
    const char*    topic;
    const uint8_t* body;
    size_t         body_len;
} esc_event_t;

// Forward declaration — implemented in Go below via //export.
extern int goCallbackTrampoline(const esc_event_t* event, void* user_data);
*/
import "C"

import (
"sync"
"unsafe"
)

// ── Callback registry ─────────────────────────────────────────────────────
// cgo prohibits passing Go function values directly across the FFI boundary.
// We store callbacks in a global map keyed by an integer index and pass that
// index as the user_data void*.

var (
cbMu       sync.Mutex
cbRegistry = map[int]EventCallback{}
cbNext     = 0
)

func registerCallback(fn EventCallback) int {
cbMu.Lock()
defer cbMu.Unlock()
idx := cbNext
cbNext++
cbRegistry[idx] = fn
return idx
}

//export goCallbackTrampoline
func goCallbackTrampoline(event *C.esc_event_t, userData unsafe.Pointer) C.int {
idx := int(uintptr(userData))

cbMu.Lock()
fn, ok := cbRegistry[idx]
cbMu.Unlock()

if !ok {
return 1 // unsubscribe if registry entry missing
}

topic := ""
if event.topic != nil {
topic = C.GoString(event.topic)
}

var body []byte
if event.body != nil && event.body_len > 0 {
body = C.GoBytes(unsafe.Pointer(event.body), C.int(event.body_len))
}

evt := Event{
ID:       uint32(event.id),
Priority: Priority(event.priority),
Topic:    topic,
Body:     body,
}

keep := fn(evt)
if !keep {
cbMu.Lock()
delete(cbRegistry, idx)
cbMu.Unlock()
return 1 // signal unsubscribe to engine
}
return 0
}
