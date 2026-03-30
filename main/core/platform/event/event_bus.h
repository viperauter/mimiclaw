#pragma once

#include "io_buf.h"
#include "queue.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * Event Bus - Cross-thread message transport
 * ==================================================================== */

/* Event types */
typedef enum {
    EVENT_NONE = 0,

    /* Receive path: Reactor -> Worker */
    EVENT_CONNECT,      /* Connection established */
    EVENT_DISCONNECT,   /* Connection closed */
    EVENT_RECV,         /* Data received */
    EVENT_ERROR,        /* Error occurred */

    /* Send path: Worker -> Reactor */
    EVENT_SEND,         /* Request to send data */
    EVENT_CLOSE,        /* Request to close connection */
    EVENT_CALL,         /* Request to run a callback in reactor thread */

    /* Timer */
    EVENT_TIMER,        /* Timer fired */
} event_type_t;

/* Connection type */
typedef enum {
    CONN_UNKNOWN = 0,
    CONN_WS_CLIENT,
    CONN_WS_SERVER,
    CONN_HTTP_CLIENT,
    CONN_STDIO,
} conn_type_t;

/* Message flags */
#define EVENT_FLAG_URGENT      (1 << 0)  /* Urgent message */
#define EVENT_FLAG_INTERNAL    (1 << 1)  /* Internal message */
#define EVENT_FLAG_RETRY       (1 << 2)  /* Retryable message */
#define EVENT_FLAG_NO_COPY     (1 << 3)  /* No copy needed */
#define EVENT_FLAG_BINARY      (1 << 4)  /* WebSocket binary frame */

/* Connection ID conversion macros */
#define CONN_TO_ID(ptr) ((uint64_t)(uintptr_t)(ptr))
#define ID_TO_CONN(id)  ((void *)(uintptr_t)(id))

/* --------------------------------------------------------------------
 * Event message structure
 * -------------------------------------------------------------------- */
typedef struct {
    uint64_t conn_id;       /* Opaque connection handle */
    uint64_t user_data;     /* User context */
    uint64_t timestamp_ns;  /* Timestamp in nanoseconds */
    event_type_t type;      /* Event type */
    conn_type_t conn_type;  /* Connection type */
    uint32_t flags;         /* Message flags */
    int32_t error_code;     /* Optional error code */
    io_buf_t *buf;          /* Refcounted data buffer (can be NULL) */
} event_msg_t;

/* --------------------------------------------------------------------
 * Event Bus - main handle
 * -------------------------------------------------------------------- */
typedef struct event_bus event_bus_t;

/* Create / destroy event bus */
event_bus_t *event_bus_create(size_t queue_capacity);
void event_bus_destroy(event_bus_t *bus);

/* Get queues (mainly for internal use) */
mimi_queue_t *event_bus_get_recv_queue(event_bus_t *bus);
mimi_queue_t *event_bus_get_send_queue(event_bus_t *bus);

/* --------------------------------------------------------------------
 * Post events (non-blocking)
 * -------------------------------------------------------------------- */

/* Reactor -> Worker (recv) */
int event_bus_post_recv(event_bus_t *bus,
                        event_type_t type,
                        uint64_t conn_id,
                        conn_type_t conn_type,
                        io_buf_t *buf,
                        uint64_t user_data,
                        uint32_t flags);

/* Worker -> Reactor (send) */
int event_bus_post_send(event_bus_t *bus,
                        uint64_t conn_id,
                        conn_type_t conn_type,
                        io_buf_t *buf,
                        uint32_t flags);

/* Worker -> Reactor: request close */
int event_bus_post_close(event_bus_t *bus,
                         uint64_t conn_id,
                         conn_type_t conn_type,
                         uint32_t flags);

/* Worker -> Reactor: run a callback in reactor/event-loop thread */
typedef void (*event_bus_call_fn_t)(void *arg);
int event_bus_post_call(event_bus_t *bus, event_bus_call_fn_t fn, void *arg);

/* Post error events */
int event_bus_post_error(event_bus_t *bus,
                         uint64_t conn_id,
                         conn_type_t conn_type,
                         int32_t error_code,
                         uint64_t user_data,
                         uint32_t flags);

/* --------------------------------------------------------------------
 * Wakeup mechanism (optional)
 * Allows worker threads to wake the reactor immediately
 * -------------------------------------------------------------------- */
typedef void (*event_bus_wakeup_fn)(void *arg);
void event_bus_set_wakeup(event_bus_t *bus,
                          event_bus_wakeup_fn fn,
                          void *arg);

/* --------------------------------------------------------------------
 * Drain queues (called from reactor thread)
 * -------------------------------------------------------------------- */
void event_bus_drain_recv(event_bus_t *bus);
void event_bus_drain_send(event_bus_t *bus);

/* --------------------------------------------------------------------
 * Global event bus instance
 * -------------------------------------------------------------------- */
event_bus_t *event_bus_get_global(void);
void event_bus_set_global(event_bus_t *bus);

#ifdef __cplusplus
}
#endif