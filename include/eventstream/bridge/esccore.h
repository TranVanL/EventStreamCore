/**
 * @file esccore.h
 * @brief EventStreamCore — Public C API
 *
 * This is the **universal interface** to the EventStreamCore engine.
 * Any language that can call C functions (Python via ctypes, Go via cgo,
 * Rust via FFI, Java via JNI, …) can drive the engine through this API.
 *
 * Thread safety:
 *   - esccore_init / esccore_shutdown must be called from a single thread.
 *   - All other functions are safe to call concurrently from any thread.
 *
 * Lifetime:
 *   esccore_init()  →  esccore_push / esccore_subscribe / …  →  esccore_shutdown()
 *
 * @version 1.0
 */

#ifndef ESCCORE_H
#define ESCCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Error codes                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    ESC_OK            =  0,
    ESC_ERR_INIT      = -1,   /**< Engine not initialised or already shut down */
    ESC_ERR_BACKPRES  = -2,   /**< Backpressure — try again later              */
    ESC_ERR_INVALID   = -3,   /**< Invalid argument (null pointer, bad size)   */
    ESC_ERR_TIMEOUT   = -4,   /**< Operation timed out                         */
    ESC_ERR_FULL      = -5,   /**< Queue is full                               */
    ESC_ERR_INTERNAL  = -99   /**< Internal / unexpected failure               */
} esc_status_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Event priority (mirrors EventStream::EventPriority)                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    ESC_PRIORITY_BATCH       = 0,
    ESC_PRIORITY_LOW         = 1,
    ESC_PRIORITY_MEDIUM      = 2,
    ESC_PRIORITY_HIGH        = 3,
    ESC_PRIORITY_CRITICAL    = 4
} esc_priority_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Event descriptor (flat, no heap — safe across FFI boundary)               */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t       id;                /**< Caller-assigned event ID            */
    esc_priority_t priority;          /**< Routing priority                    */
    const char*    topic;             /**< Null-terminated topic string        */
    const uint8_t* body;              /**< Payload bytes (may be NULL)         */
    size_t         body_len;          /**< Payload length in bytes             */
} esc_event_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Metrics snapshot (read-only, filled by esccore_metrics)                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t total_events_pushed;
    uint64_t total_events_processed;
    uint64_t total_events_dropped;
    uint64_t realtime_queue_depth;
    uint64_t transactional_queue_depth;
    uint64_t batch_queue_depth;
    uint64_t dlq_depth;
    int      backpressure_level;      /**< 0=NORMAL … 4=EMERGENCY             */
} esc_metrics_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Health snapshot                                                           */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int  is_alive;                    /**< 1 = process alive                   */
    int  is_ready;                    /**< 1 = pipeline running                */
    int  backpressure_level;          /**< 0=NORMAL … 4=EMERGENCY             */
} esc_health_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Lifecycle                                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialise the engine.
 * @param config_path  Path to YAML config file (NULL → "config/config.yaml")
 * @return ESC_OK on success.
 */
esc_status_t esccore_init(const char* config_path);

/**
 * @brief Graceful shutdown.  Drains in-flight events then stops all threads.
 */
esc_status_t esccore_shutdown(void);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Publish                                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Push a single event into the engine.
 *
 * The caller retains ownership of the esc_event_t and its pointed-to
 * buffers — the engine copies what it needs before returning.
 *
 * @return ESC_OK, ESC_ERR_BACKPRES, or ESC_ERR_FULL.
 */
esc_status_t esccore_push(const esc_event_t* event);

/**
 * @brief Push a batch of events (more efficient than N × esccore_push).
 * @param events  Array of esc_event_t.
 * @param count   Number of events in the array.
 * @param pushed  [out] How many were actually pushed (may be < count).
 * @return ESC_OK if all pushed, ESC_ERR_BACKPRES if partially pushed.
 */
esc_status_t esccore_push_batch(const esc_event_t* events,
                                size_t count,
                                size_t* pushed);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Subscribe (callback-based)                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Callback invoked for each processed event.
 * Return 0 to continue, non-zero to unsubscribe.
 */
typedef int (*esc_event_callback_t)(const esc_event_t* event, void* user_data);

/**
 * @brief Register a callback for processed events.
 * @param topic_prefix  Topic prefix filter (NULL = all topics).
 * @param cb            Callback function.
 * @param user_data     Opaque pointer forwarded to every invocation.
 * @return ESC_OK on success.
 */
esc_status_t esccore_subscribe(const char* topic_prefix,
                               esc_event_callback_t cb,
                               void* user_data);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Observability                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Fill a metrics snapshot.
 */
esc_status_t esccore_metrics(esc_metrics_t* out);

/**
 * @brief Fill a health snapshot.
 */
esc_status_t esccore_health(esc_health_t* out);

/**
 * @brief Get a Prometheus-compatible text exposition.
 * @param buf     Caller-owned buffer.
 * @param buf_len Size of buffer.
 * @param written [out] Bytes written (excluding NUL).
 */
esc_status_t esccore_metrics_prometheus(char* buf, size_t buf_len, size_t* written);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Pipeline control                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Pause the pipeline (events accumulate but are not processed).
 */
esc_status_t esccore_pause(void);

/**
 * @brief Resume the pipeline.
 */
esc_status_t esccore_resume(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ESCCORE_H */
