# MimiClaw Event-Driven Architecture Refactoring Plan

## Executive Summary

This refactoring addresses the core issue: **blocking operations in the event loop** and **thread safety of I/O operations**. The solution introduces:

1. **recv_queue** - For dispatching received data to worker threads
2. **send_queue** - For sending data from worker threads to event loop
3. **io_buf** - Zero-copy reference-counted buffer

All I/O operations (send/recv) happen in the event loop thread only.

---

## Part 1: Problem Analysis

### 1.1 Current Architecture Issues

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Application Layer                           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                 │
│  │   Channel   │  │   Channel   │  │   Channel   │                 │
│  │  (Feishu)   │  │ (Telegram)  │  │    (CLI)    │                 │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘                 │
│         │                │                │                         │
│         └────────────────┼────────────────┘                         │
│                          │                                          │
│                    ┌─────▼─────┐                                    │
│                    │   Router  │  ◄── Gateway to Channel routing    │
│                    └─────┬─────┘                                    │
│                          │                                          │
├──────────────────────────┼──────────────────────────────────────────┤
│                    ┌─────▼─────┐                                    │
│                    │  Gateway  │  ◄── Protocol abstraction          │
│                    │  Manager  │                                    │
│                    └─────┬─────┘                                    │
│         ┌────────────────┼────────────────┐                         │
│         │                │                │                         │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐                 │
│  │ WS Client   │  │ WS Server   │  │ HTTP Client │                 │
│  │  Gateway    │  │  Gateway    │  │  Gateway    │                 │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘                 │
│         │                │                │                         │
├─────────┼────────────────┼────────────────┼─────────────────────────┤
│         │                │                │                         │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐                 │
│  │ platform/   │  │ platform/   │  │ platform/   │                 │
│  │ websocket   │  │ websocket   │  │    http     │  ◄── EXISTING   │
│  │ (client)    │  │ (server)    │  │             │      ABSTRACTION│
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘                 │
│         │                │                │                         │
│         └────────────────┼────────────────┘                         │
│                          │                                          │
│                    ┌─────▼─────┐                                    │
│                    │  Runtime  │  ◄── Event loop thread (mongoose)  │
│                    │  (mg_mgr) │                                    │
│                    └───────────┘                                    │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.2 Two Core Problems

**Problem 1: Blocking in Event Loop**
```
Event Loop Thread (mg_mgr_poll)
       │
       ▼
  mongoose callback
       │
       ▼
  gateway callback (on_message_cb)
       │
       ▼
  ❌ BLOCKING: HTTP requests, JSON parsing, etc.
```

**Problem 2: Thread Safety**
```
Worker Thread                    Event Loop Thread
       │                              │
       ▼                              │
  gateway_send()                      │
       │                              │
       ▼                              │
  mg_ws_send()  ◄─────────────────────┼── mg_mgr_poll()
       │                              │
       ▼                              ▼
  ❌ RACE CONDITION: Both threads access mongoose internals
```

**Root Cause**: 
1. Business logic executes in event loop thread
2. I/O operations called from worker threads

---

## Part 2: Target Architecture

### 2.1 New Architecture - All I/O in Event Loop

```
┌─────────────────────────────────────────────────────────────────────┐
│                                                                      │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │                   Event Loop Thread                         │   │
│   │                                                             │   │
│   │   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    │   │
│   │   │ mg_mgr_poll │    │  recv_queue │    │  send_queue │    │   │
│   │   │             │    │  (dispatch) │    │  (process)  │    │   │
│   │   └──────┬──────┘    └──────┬──────┘    └──────┬──────┘    │   │
│   │          │                  │                  │            │   │
│   │          │                  ▼                  ▼            │   │
│   │          │           ┌───────────┐      ┌───────────┐       │   │
│   │          │           │ Dispatch  │      │  Execute  │       │   │
│   │          │           │ to worker │      │  mg_send  │       │   │
│   │          │           └───────────┘      └───────────┘       │   │
│   │          │                                                   │   │
│   │          ▼                                                   │   │
│   │   ┌─────────────┐                                           │   │
│   │   │   mongoose  │  ◄── ALL I/O happens here                 │   │
│   │   │  callbacks  │                                           │   │
│   │   └──────┬──────┘                                           │   │
│   │          │                                                   │   │
│   └──────────┼───────────────────────────────────────────────────┘   │
│              │                                                       │
│              │ push to recv_queue                                    │
│              │                                                       │
│   ┌──────────▼───────────────────────────────────────────────────┐   │
│   │                   Worker Thread Pool                          │   │
│   │                                                               │   │
│   │   ┌─────────────────┐      ┌─────────────────┐               │   │
│   │   │  Business Logic │      │  gateway_send() │               │   │
│   │   │  • JSON parse   │      │  push to        │               │   │
│   │   │  • HTTP request │      │  send_queue     │               │   │
│   │   │  • Router call  │      │                 │               │   │
│   │   └─────────────────┘      └─────────────────┘               │   │
│   │                                                               │   │
│   └───────────────────────────────────────────────────────────────┘   │
│                                                                       │
└───────────────────────────────────────────────────────────────────────┘
```

### 2.2 Data Flow - Receive Path (Zero-Copy)

```
Event Loop Thread                    Worker Thread
       │                                   │
       ▼                                   │
  mg_ws_message                           │
       │                                   │
       ▼                                   │
  io_buf_from_const()                     │
  io_buf_ref(buf)                         │
       │                                   │
       ▼                                   │
  event_msg_post(RECV)                    │
       │                                   │
       ▼                                   │
  push to recv_queue  ───────────────────►│
       │                                   ▼
       │                            pop from recv_queue
       │                                   │
       │                                   ▼
       │                            gateway_handler(msg)
       │                                   │
       │                                   ▼
       │                            io_buf_unref(buf)
       │                                   │
       ▼                                   ▼
  (continue polling)                 (business logic)
```

### 2.3 Data Flow - Send Path (Zero-Copy)

```
Worker Thread                        Event Loop Thread
       │                                   │
       ▼                                   │
  gateway_send(content)                    │
       │                                   │
       ▼                                   │
  io_buf_from_const(content)               │
       │                                   │
       ▼                                   │
  event_msg_post(SEND)                     │
       │                                   │
       ▼                                   │
  push to send_queue  ────────────────────►│
       │                                   ▼
       │                            pop from send_queue
       │                                   │
       │                                   ▼
       │                            mg_ws_send(buf->data)
       │                                   │
       │                                   ▼
       │                            io_buf_unref(buf)
       │                                   │
       ▼                                   ▼
  (continue work)                    (continue polling)
```

---

## Part 3: Core Components

### 3.1 Design Considerations for Multi-Backend Support

To support multiple backends (mongoose, libuv, libevent), we need to consider:

| Backend | Connection Handle | Buffer Type | Write Request |
|---------|------------------|-------------|---------------|
| Mongoose | `mg_connection*` | `mg_str` | Direct call |
| libuv | `uv_stream_t*` | `uv_buf_t` | `uv_write_t` (must persist until callback) |
| libevent | `bufferevent*` | `evbuffer` | Copies data internally |

**Key Challenges:**
1. **libuv** requires `uv_write_t` to remain valid until write completes
2. **libevent** has its own buffer management (`evbuffer`)
3. **mongoose** uses `mg_str` which is just a pointer + length

**Solution:** Use a unified `io_buf_t` with backend-specific free callback.

### 3.2 io_buf - Zero-Copy Buffer

```c
/* main/platform/io_buf.h */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct io_buf io_buf_t;

/* Buffer free callback - backend implements this */
typedef void (*io_buf_free_fn)(io_buf_t *buf);

struct io_buf {
    uint8_t *base;          /* Pointer to data (named 'base' for libuv compatibility) */
    size_t len;             /* Data length */
    
    /* Reference counting for zero-copy */
    volatile int refcount;
    
    /* Free callback - backend-specific */
    io_buf_free_fn free_fn;
    
    /* Backend-specific data */
    union {
        void *ptr;          /* Generic pointer */
        struct {
            void *backend_data;  /* Backend-specific (e.g., uv_write_t for libuv) */
            uint32_t flags;      /* Backend flags */
        };
    };
};

/**
 * Allocate a new io_buf with data buffer.
 * Returns buffer with refcount = 1.
 */
io_buf_t *io_buf_alloc(size_t capacity);

/**
 * Create io_buf from existing data (takes ownership).
 * Returns buffer with refcount = 1.
 */
io_buf_t *io_buf_from_data(void *data, size_t len);

/**
 * Create io_buf from const data (copies data).
 * Returns buffer with refcount = 1.
 */
io_buf_t *io_buf_from_const(const void *data, size_t len);

/**
 * Create io_buf for backend-specific buffer.
 * Backend provides free_fn to handle its own buffer type.
 */
io_buf_t *io_buf_wrap(void *data, size_t len, io_buf_free_fn free_fn, void *backend_data);

/**
 * Increment reference count.
 * Returns the same pointer for convenience.
 */
io_buf_t *io_buf_ref(io_buf_t *buf);

/**
 * Decrement reference count.
 * Calls free_fn when refcount reaches 0.
 */
void io_buf_unref(io_buf_t *buf);

/**
 * Get libuv-compatible buffer (uv_buf_t).
 * For libuv backend compatibility.
 */
typedef struct {
    char *base;
    size_t len;
} uv_buf_t;

static inline uv_buf_t io_buf_to_uv(io_buf_t *buf) {
    uv_buf_t uv = { .base = (char *)buf->base, .len = buf->len };
    return uv;
}

#ifdef __cplusplus
}
#endif
```

### 3.3 event_msg - Lightweight Event Message

```c
/* main/platform/event_msg.h */

#pragma once

#include "io_buf.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event types */
typedef enum {
    EVENT_MSG_NONE = 0,
    
    /* Receive path: Event Loop -> Worker */
    EVENT_MSG_CONNECT,      /* Connection established */
    EVENT_MSG_DISCONNECT,   /* Connection closed */
    EVENT_MSG_RECV,         /* Data received */
    EVENT_MSG_ERROR,        /* Error occurred */
    
    /* Send path: Worker -> Event Loop */
    EVENT_MSG_SEND,         /* Request to send data */
    EVENT_MSG_CLOSE,        /* Request to close connection */
    
    /* Timer */
    EVENT_MSG_TIMER,        /* Timer fired */
} event_msg_type_t;

/* Connection type - identifies gateway type */
typedef enum {
    EVENT_CONN_UNKNOWN = 0,
    EVENT_CONN_WS_CLIENT,
    EVENT_CONN_WS_SERVER,
    EVENT_CONN_HTTP_CLIENT,
    EVENT_CONN_STDIO,
} event_conn_type_t;

/*
 * Event message - lightweight, portable
 * 
 * conn_id: Opaque connection handle
 *   - Mongoose: (uint64_t)mg_connection*
 *   - libuv:    (uint64_t)uv_stream_t*
 *   - libevent: (uint64_t)bufferevent*
 */
typedef struct {
    uint64_t conn_id;       /* Connection handle (opaque pointer cast) */
    uint64_t user_data;     /* User context (as integer, or pointer cast) */
    uint32_t type;          /* event_msg_type_t */
    uint32_t conn_type;     /* event_conn_type_t */
    io_buf_t *buf;          /* Data buffer (refcounted, can be NULL) */
    int error_code;         /* Error code for EVENT_MSG_ERROR */
} event_msg_t;

/* Initialize/shutdown event message system */
int event_msg_system_init(size_t queue_capacity);
void event_msg_system_deinit(void);

/* Get queues */
mimi_queue_t *event_msg_get_recv_queue(void);
mimi_queue_t *event_msg_get_send_queue(void);

/* Post event to recv queue (called from event loop) */
int event_msg_post_recv(event_msg_type_t type, uint64_t conn_id,
                        event_conn_type_t conn_type, io_buf_t *buf,
                        uint64_t user_data);

/* Post event to send queue (called from worker threads) */
int event_msg_post_send(uint64_t conn_id, event_conn_type_t conn_type,
                        io_buf_t *buf);

/* Post close request to send queue */
int event_msg_post_close(uint64_t conn_id, event_conn_type_t conn_type);

/* Post error event */
int event_msg_post_error(uint64_t conn_id, event_conn_type_t conn_type,
                         int error_code, uint64_t user_data);

#ifdef __cplusplus
}
#endif
```

### 3.4 Event Dispatcher - Worker Pool

```c
/* main/platform/event_dispatcher.h */

#pragma once

#include "event_msg.h"
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event handler callback - gateway implements this */
typedef void (*event_handler_t)(event_msg_t *msg, void *user_data);

/* Send handler callback - platform layer implements this */
typedef void (*event_send_handler_t)(event_msg_t *msg);

/* Dispatcher */
typedef struct event_dispatcher event_dispatcher_t;

/* Create dispatcher with worker threads */
event_dispatcher_t *event_dispatcher_create(size_t worker_count);

/* Destroy dispatcher */
void event_dispatcher_destroy(event_dispatcher_t *disp);

/* Register a handler for a connection type (for recv events) */
int event_dispatcher_register(event_dispatcher_t *disp,
                              event_conn_type_t conn_type,
                              event_handler_t handler,
                              void *user_data);

/* Register send handler (called in event loop for send events) */
int event_dispatcher_register_send_handler(event_conn_type_t conn_type,
                                           event_send_handler_t handler);

/* Start/stop dispatcher workers */
int event_dispatcher_start(event_dispatcher_t *disp);
void event_dispatcher_stop(event_dispatcher_t *disp);

/* Get global dispatcher instance */
event_dispatcher_t *event_dispatcher_get(void);

/* Process send queue (called from event loop) */
void event_dispatcher_process_send_queue(event_dispatcher_t *disp);

#ifdef __cplusplus
}
#endif
```

---

## Part 4: Integration with Existing Code

### 4.1 Platform WebSocket Layer - Receive Path

```c
/* main/platform/websocket/mg_impl/websocket.c */

static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data)
{
    mimi_ws_ctx_t *ctx = c->fn_data;
    
    switch (ev) {
        case MG_EV_WS_MSG: {
            struct mg_ws_message *wm = ev_data;
            
            /* Create io_buf from mongoose data */
            io_buf_t *buf = io_buf_from_const(wm->data.buf, wm->data.len);
            if (!buf) return;
            
            /* Post to recv queue - NON-BLOCKING */
            event_msg_post_recv(EVENT_MSG_RECV, 
                               (uint64_t)c,  /* conn_id = mg_connection pointer */
                               EVENT_CONN_WS_CLIENT, 
                               buf, 
                               (uint64_t)ctx);
            
            /* Unref our reference - queue holds one */
            io_buf_unref(buf);
            break;
        }
        
        case MG_EV_CLOSE: {
            event_msg_post_recv(EVENT_MSG_DISCONNECT, 
                               (uint64_t)c,
                               EVENT_CONN_WS_CLIENT, 
                               NULL, 
                               (uint64_t)ctx);
            break;
        }
    }
}
```

### 4.2 Send Queue Processing - In Event Loop

```c
/* main/platform/event_dispatcher.c */

void event_dispatcher_process_send_queue(event_dispatcher_t *disp)
{
    event_msg_t msg;
    
    while (mimi_queue_try_recv(disp->send_queue, &msg) == MIMI_OK) {
        
        switch (msg.type) {
            case EVENT_MSG_SEND: {
                /* Cast conn_id back to mg_connection */
                struct mg_connection *c = (struct mg_connection *)msg.conn_id;
                
                if (c && msg.buf) {
                    /* Send via mongoose - called in event loop thread */
                    mg_ws_send(c, (const char *)msg.buf->base, msg.buf->len, 
                              WEBSOCKET_OP_TEXT);
                }
                
                /* Free buffer */
                if (msg.buf) io_buf_unref(msg.buf);
                break;
            }
            
            case EVENT_MSG_CLOSE: {
                struct mg_connection *c = (struct mg_connection *)msg.conn_id;
                if (c) {
                    mg_close_conn(c);
                }
                if (msg.buf) io_buf_unref(msg.buf);
                break;
            }
            
            default:
                if (msg.buf) io_buf_unref(msg.buf);
                break;
        }
    }
}
```

### 4.3 Gateway - Send from Worker Thread

```c
/* main/gateway/websocket/ws_client_gateway.c */

static mimi_err_t ws_client_gateway_send_impl(gateway_t *gw, const char *session_id,
                                              const char *content)
{
    (void)session_id;
    
    if (!gw || !gw->priv_data || !content) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    ws_client_gateway_priv_t *priv = gw->priv_data;
    
    if (!priv->connected || !priv->ws_conn) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    /* Create io_buf from content */
    io_buf_t *buf = io_buf_from_const(content, strlen(content));
    if (!buf) {
        return MIMI_ERR_NO_MEM;
    }
    
    /* Post to send queue - will be processed in event loop */
    int ret = event_msg_post_send((uint64_t)priv->ws_conn, 
                                  EVENT_CONN_WS_CLIENT, 
                                  buf);
    
    /* Unref our reference - queue holds one */
    io_buf_unref(buf);
    
    return (ret == 0) ? MIMI_OK : MIMI_ERR_IO;
}
```

### 4.4 Runtime - Process Both Queues

```c
/* main/platform/runtime.c */

static void event_loop_thread(void *arg)
{
    while (g_runtime.running) {
        /* Poll mongoose with short timeout */
        mg_mgr_poll(&g_runtime.mgr, 10);  /* 10ms timeout */
        
        /* Process send queue - execute pending sends */
        event_dispatcher_process_send_queue(g_runtime.dispatcher);
    }
}

mimi_err_t mimi_runtime_init(void)
{
    mg_mgr_init(&g_runtime.mgr);
    
    /* Initialize event message system with two queues */
    event_msg_system_init(64);  /* 64 items per queue */
    
    /* Create dispatcher with 2-4 workers */
    g_runtime.dispatcher = event_dispatcher_create(2);
    
    return MIMI_OK;
}

mimi_err_t mimi_runtime_start(void)
{
    g_runtime.running = true;
    
    /* Start dispatcher workers */
    event_dispatcher_start(g_runtime.dispatcher);
    
    /* Create event loop thread */
    mimi_task_create(&g_runtime.thread, event_loop_thread, NULL);
    
    return MIMI_OK;
}
```

### 4.5 Gateway Handler Registration

```c
/* main/gateway/websocket/ws_client_gateway.c */

static void ws_client_event_handler(event_msg_t *msg, void *user_data)
{
    ws_client_gateway_priv_t *priv = find_priv_by_conn_id(msg->conn_id);
    if (!priv) {
        if (msg->buf) io_buf_unref(msg->buf);
        return;
    }
    
    switch (msg->type) {
        case EVENT_MSG_RECV:
            /* Now we're in worker thread - OK to do blocking operations */
            if (msg->buf && priv->gateway->on_message_cb) {
                char *content = (char *)msg->buf->base;
                priv->gateway->on_message_cb(priv->gateway, "default",
                                            content, 
                                            priv->gateway->callback_user_data);
            }
            break;
            
        case EVENT_MSG_CONNECT:
            priv->connected = true;
            if (priv->gateway->on_connect_cb) {
                priv->gateway->on_connect_cb(priv->gateway, "default",
                                            priv->gateway->callback_user_data);
            }
            break;
            
        case EVENT_MSG_DISCONNECT:
            priv->connected = false;
            if (priv->gateway->on_disconnect_cb) {
                priv->gateway->on_disconnect_cb(priv->gateway, "default",
                                               priv->gateway->callback_user_data);
            }
            break;
    }
    
    if (msg->buf) io_buf_unref(msg->buf);
}

mimi_err_t ws_client_gateway_module_init(void)
{
    /* ... existing init ... */
    
    /* Register recv handler with dispatcher */
    event_dispatcher_register(event_dispatcher_get(),
                              EVENT_CONN_WS_CLIENT,
                              ws_client_event_handler,
                              NULL);
    
    return MIMI_OK;
}
```

---

## Part 5: Future Backend Migration

When migrating to libuv or libevent, only the **send queue processing** needs to change:

### 5.1 libuv Send Processing (Future)

```c
/* For libuv, uv_write_t must persist until callback */

typedef struct {
    uv_write_t req;
    io_buf_t *buf;
} uv_write_ctx_t;

static void uv_write_done(uv_write_t *req, int status) {
    uv_write_ctx_t *ctx = (uv_write_ctx_t *)req->data;
    io_buf_unref(ctx->buf);
    free(ctx);
}

void event_dispatcher_process_send_queue(event_dispatcher_t *disp)
{
    event_msg_t msg;
    
    while (mimi_queue_try_recv(disp->send_queue, &msg) == MIMI_OK) {
        if (msg.type == EVENT_MSG_SEND && msg.buf) {
            uv_stream_t *stream = (uv_stream_t *)msg.conn_id;
            
            /* Allocate context that persists until callback */
            uv_write_ctx_t *ctx = malloc(sizeof(*ctx));
            ctx->buf = io_buf_ref(msg.buf);  /* Hold reference */
            ctx->req.data = ctx;
            
            uv_buf_t uv_buf = { .base = (char *)msg.buf->base, .len = msg.buf->len };
            uv_write(&ctx->req, stream, &uv_buf, 1, uv_write_done);
        }
        io_buf_unref(msg.buf);
    }
}
```

### 5.2 libevent Send Processing (Future)

```c
/* libevent copies data internally, simpler than libuv */

void event_dispatcher_process_send_queue(event_dispatcher_t *disp)
{
    event_msg_t msg;
    
    while (mimi_queue_try_recv(disp->send_queue, &msg) == MIMI_OK) {
        if (msg.type == EVENT_MSG_SEND && msg.buf) {
            struct bufferevent *bev = (struct bufferevent *)msg.conn_id;
            
            /* libevent copies data internally */
            bufferevent_write(bev, msg.buf->base, msg.buf->len);
        }
        io_buf_unref(msg.buf);
    }
}
```

### 5.3 Migration Path

```
Current:  Mongoose only, direct cast in process_send_queue()
          │
          ▼
Future:   Add #ifdef or runtime dispatch for libuv/libevent
          │
          ▼
          Only process_send_queue() changes, rest of code unchanged
```

---

## Part 6: Router Layer - Unchanged

The router layer handles routing between gateways and channels. It operates at a higher level and does NOT need to integrate with the dispatcher.

```
┌─────────────────┐
│    Gateway      │
│   (callback)    │  ◄── Runs in worker thread now
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│     Router      │  ◄── Routes messages to correct channel
│                 │      Based on gateway name mapping
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│    Channel      │
│  (Feishu/etc)   │  ◄── Runs in worker thread
└─────────────────┘
```

The router is called from gateway callbacks (which now run in worker threads), so it naturally benefits from the non-blocking architecture without any changes.

---

## Part 6: Implementation Plan

### Phase 1: Core Infrastructure (Week 1)

**Files to create:**
| File | Purpose |
|------|---------|
| `main/platform/io_buf.h` | Zero-copy buffer interface |
| `main/platform/io_buf.c` | Buffer implementation with refcount |
| `main/platform/event_msg.h` | Event message definition |
| `main/platform/event_msg.c` | Event message system with two queues |
| `main/platform/event_dispatcher.h` | Dispatcher interface |
| `main/platform/event_dispatcher.c` | Worker pool implementation |

**Files to modify:**
| File | Changes |
|------|---------|
| `main/platform/runtime.h` | Add dispatcher handle |
| `main/platform/runtime.c` | Initialize dispatcher, process send queue |

### Phase 2: Platform Layer Migration (Week 2)

**Files to modify:**
| File | Changes |
|------|---------|
| `main/platform/websocket/mg_impl/websocket.c` | Post events for recv, register send handler |
| `main/platform/http/mg_impl/http.c` | Post events for async HTTP |

### Phase 3: Gateway Migration (Week 3)

**Files to modify:**
| File | Changes |
|------|---------|
| `main/gateway/websocket/ws_client_gateway.c` | Register handler, use send queue |
| `main/gateway/websocket/ws_server_gateway.c` | Register handler, use send queue |
| `main/gateway/http/http_gateway.c` | Register handler, use send queue |

### Phase 4: Testing (Week 4)

- Integration testing
- Thread safety verification
- Performance benchmarking
- Memory leak checking

---

## Part 7: File Summary

### New Files (6 files)

```
main/platform/
├── io_buf.h              # Zero-copy buffer interface
├── io_buf.c              # Buffer implementation
├── event_msg.h           # Event message definition
├── event_msg.c           # Event message system (recv_queue + send_queue)
├── event_dispatcher.h    # Dispatcher interface
└── event_dispatcher.c    # Worker pool implementation
```

### Modified Files (5 files)

```
main/platform/
├── runtime.h          # Add dispatcher handle
├── runtime.c          # Initialize/dispatcher lifecycle, process send queue
└── websocket/mg_impl/
    └── websocket.c    # Post recv events, store conn_id

main/gateway/websocket/
├── ws_client_gateway.c  # Register handler, use send queue
└── ws_server_gateway.c  # Register handler, use send queue
```

### Unchanged Files

- `main/router/*` - Router layer unchanged
- `main/channels/*` - Channel layer unchanged
- `main/gateway/gateway.h` - Gateway interface unchanged
- `main/platform/websocket/websocket.h` - WebSocket interface unchanged

---

## Part 8: Success Criteria

1. **Non-blocking event loop**: `mg_mgr_poll()` completes within 1ms
2. **Thread safety**: All I/O operations happen in event loop thread only
3. **Zero-copy data path**: Data copied at most once (from mongoose to io_buf)
4. **Clean separation**: Platform layer has no business logic
5. **Minimal changes**: Gateway/Channel/Router interfaces unchanged
6. **Performance**: Message throughput > 10K msg/sec

---

## Appendix A: Why Two Queues?

```
recv_queue:  Event Loop ──► Worker Threads
send_queue:  Worker Threads ──► Event Loop
```

**recv_queue**: Dispatches received data to worker threads for processing
**send_queue**: Collects send requests from worker threads, executes in event loop

This ensures:
1. All I/O operations happen in one thread (event loop)
2. Worker threads never call backend directly
3. No locks needed around backend internals

---

## Appendix B: Why io_buf?

Without io_buf, data would be copied multiple times:

```
backend buffer ──copy──► event_msg.data ──copy──► handler
```

With io_buf, we achieve zero-copy:

```
backend buffer ──ref──► io_buf ──ref──► queue ──ref──► handler ──unref──► free
```

The reference counting ensures the buffer is freed only when all references are released.

---

## Appendix C: Connection ID Mapping

We use connection pointer as connection ID:

```c
/* Mongoose */
uint64_t conn_id = (uint64_t)mg_connection_ptr;
mg_connection *c = (mg_connection *)conn_id;

/* libuv (future) */
uint64_t conn_id = (uint64_t)uv_stream_ptr;
uv_stream_t *s = (uv_stream_t *)conn_id;

/* libevent (future) */
uint64_t conn_id = (uint64_t)bufferevent_ptr;
bufferevent *bev = (bufferevent *)conn_id;
```

This works because:
1. Connection pointers are unique while connection is alive
2. Send handler validates connection is still valid before sending

---

## Appendix D: CLI Channel - Unaffected

The CLI channel has its own thread and reads from stdin directly. It does not use the event loop and is unaffected by this refactoring.
