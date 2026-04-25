/**
 * @file esccore.h
 * @brief EventStreamCore — Public C API
 *
 * Minimal interface for receiving processed events from the engine.
 * Python (ctypes) and Go (cgo) register callbacks via esccore_subscribe;
 * the engine calls those callbacks as events flow out of the pipeline.
 *
 * Lifetime:
 *   esccore_init()  →  esccore_subscribe(…)  →  esccore_shutdown()
 *
 * Thread safety:
 *   - esccore_init / esccore_shutdown must be called from a single thread.
 *   - esccore_subscribe and observability calls are safe from any thread.
 *   - Callbacks are invoked from internal engine threads; keep them fast.
 *
 * @version 2.0
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
    ESC_ERR_INVALID   = -3,   /**< Invalid argument (null pointer)             */
    ESC_ERR_INTERNAL  = -99   /**< Internal / unexpected failure               */
} esc_status_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Event priority                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    ESC_PRIORITY_BATCH    = 0,
    ESC_PRIORITY_LOW      = 1,
    ESC_PRIORITY_MEDIUM   = 2,
    ESC_PRIORITY_HIGH     = 3,
    ESC_PRIORITY_CRITICAL = 4
} esc_priority_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Event descriptor (flat, safe across FFI boundary)                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t       id;        /**< Event ID assigned by the pipeline           */
    esc_priority_t priority;  /**< Routing priority                            */
    const char*    topic;     /**< Null-terminated topic string                */
    const uint8_t* body;      /**< Payload bytes (may be NULL)                 */
    size_t         body_len;  /**< Payload length in bytes                     */
} esc_event_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Metrics snapshot                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t total_events_processed;
    uint64_t total_events_dropped;
    uint64_t queue_depth;         /**< Combined queue depth                    */
    int      backpressure_level;  /**< 0=NORMAL … 4=EMERGENCY                 */
} esc_metrics_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Health snapshot                                                           */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int is_alive;             /**< 1 = process alive                           */
    int is_ready;             /**< 1 = pipeline running                        */
    int backpressure_level;   /**< 0=NORMAL … 4=EMERGENCY                     */
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
/*  Subscribe (callback-based output)                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Callback invoked for each processed event delivered by the pipeline.
 *
 * @param event      Pointer to the event (valid only for the duration of call).
 * @param user_data  Opaque pointer supplied at subscription time.
 * Return 0 to continue receiving events, non-zero to unsubscribe.
 */
typedef int (*esc_event_callback_t)(const esc_event_t* event, void* user_data);

/**
 * @brief Register a callback to receive processed events.
 *
 * Multiple subscribers can be registered; each receives every matching event.
 *
 * @param topic_prefix  Topic prefix filter — only events whose topic starts
 *                      with this string are forwarded.  Pass NULL or "" to
 *                      receive all topics.
 * @param cb            Callback function pointer.
 * @param user_data     Opaque context pointer forwarded to every invocation.
 * @return ESC_OK on success.
 */
esc_status_t esccore_subscribe(const char*          topic_prefix,
                               esc_event_callback_t cb,
                               void*                user_data);

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

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ESCCORE_H */
