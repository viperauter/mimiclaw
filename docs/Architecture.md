# MimiClaw Architecture

> Cross-platform AI Agent framework — C implementation supporting both POSIX systems (Linux/macOS) and embedded platforms (ESP32).

---

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              External Interfaces                                    │
│                                                                                     │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│   │   Telegram   │    │   WebSocket  │    │     CLI      │    │    Feishu    │      │
│   │     Bot      │    │    Server    │    │   Terminal   │    │     Bot      │      │
│   │  (HTTPS API) │    │ (WS Protocol)│    │   (STDIO)    │    │  (WS+HTTP)   │      │
│   └──────┬───────┘    └──────┬───────┘    └──────┬───────┘    └──────┬───────┘      │
└──────────┼──────────────────┼──────────────────┼──────────────────┼─────────────────┘
           │                  │                  │                  │
           ▼                  ▼                  ▼                  ▼
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              Gateway Layer                                           │
│  ┌──────────────────────────────────────────────────────────────────────────────┐   │
│  │  Transport Protocol Abstraction                                               │   │
│  │                                                                              │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │   │
│  │  │   HTTP       │  │  WebSocket   │  │    STDIO     │  │  WebSocket   │     │   │
│  │  │   Client     │  │   Server     │  │   Gateway    │  │   Client     │     │   │
│  │  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘     │   │
│  │         │                  │                  │                  │           │   │
│  │         └──────────────────┴──────────────────┴──────────────────┘           │   │
│  │                            │                                                  │   │
│  │              ┌─────────────▼──────────────┐                                   │   │
│  │              │     Gateway Manager        │                                   │   │
│  │              │  (Lifecycle & Registry)    │                                   │   │
│  │              └─────────────┬──────────────┘                                   │   │
│  └────────────────────────────┼──────────────────────────────────────────────────┘   │
└───────────────────────────────┼─────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              Channel Layer                                           │
│  ┌──────────────────────────────────────────────────────────────────────────────┐   │
│  │  Business Logic & Protocol Handling                                           │   │
│  │                                                                              │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │   │
│  │  │   Telegram   │  │  WebSocket   │  │     CLI      │  │    Feishu    │     │   │
│  │  │   Channel    │  │   Channel    │  │   Channel    │  │   Channel    │     │   │
│  │  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘     │   │
│  │         │                  │                  │                  │           │   │
│  │         └──────────────────┴──────────────────┴──────────────────┘           │   │
│  │                            │                                                  │   │
│  │              ┌─────────────▼──────────────┐                                   │   │
│  │              │     Channel Manager        │                                   │   │
│  │              │  (Lifecycle & Routing)     │                                   │   │
│  │              └─────────────┬──────────────┘                                   │   │
│  └────────────────────────────┼──────────────────────────────────────────────────┘   │
└───────────────────────────────┼─────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                           Message Bus Layer                                          │
│  ┌──────────────────────────────────────────────────────────────────────────────┐   │
│  │  Inter-Component Communication                                                │   │
│  │                                                                              │   │
│  │  ┌─────────────────────┐     ┌─────────────────────┐                        │   │
│  │  │   Inbound Queue     │     │   Outbound Queue    │                        │   │
│  │  │  (Channel→Agent)    │     │  (Agent→Channel)    │                        │   │
│  │  │                     │     │                     │                        │   │
│  │  │  • User messages    │     │  • Agent responses  │                        │   │
│  │  │  • Commands         │     │  • Tool results     │                        │   │
│  │  │  • Confirmation     │     │  • Confirm requests │                        │   │
│  │  │    responses        │     │                     │                        │   │
│  │  └──────────┬──────────┘     └──────────┬──────────┘                        │   │
│  │             │                           │                                    │   │
│  │             └───────────┬───────────────┘                                    │   │
│  │                         │                                                    │   │
│  │              ┌──────────▼──────────┐                                         │   │
│  │              │    Message Bus      │                                         │   │
│  │              │   (mimi_msg_t)      │                                         │   │
│  │              │                     │                                         │   │
│  │              │  Unified message    │                                         │   │
│  │              │  format for all     │                                         │   │
│  │              │  channels           │                                         │   │
│  │              └──────────┬──────────┘                                         │   │
│  └─────────────────────────┼────────────────────────────────────────────────────┘   │
└────────────────────────────┼────────────────────────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              Core System                                             │
│                                                                                      │
│   ┌────────────────────────┐       ┌──────────────────────┐                         │
│   │     Agent Loop          │◀─────│   Control Manager    │                         │
│   │                         │       │  (Control Channel)   │                         │
│   │  Context ──▶ LLM Proxy │       └──────────┬───────────┘                         │
│   │  Builder      (HTTPS)   │                │                                       │
│   │       ▲          │      │                │                                       │
│   │       │     tool_use?   │                │                                       │
│   │       │          ▼      │                │                                       │
│   │  Tool Results ◀─ Tools  │────▶┌──────────▼───────────┐                         │
│   │              (web_search)│    │ Tool Call Context    │                         │
│   └──────────┬─────────────┘     │    Manager           │                         │
│              │                   └──────────────────────┘                         │
│       ┌──────▼───────┐                                                              │
│       │ Outbound Queue│                                                              │
│       └──────┬───────┘                                                              │
│              │                                                                       │
│              ▼                                                                       │
│       ┌──────────────┐                                                               │
│       │   Channel    │───▶ Routes to appropriate channel                            │
│       │   Manager    │                                                               │
│       └──────────────┘                                                               │
│                                                                                      │
│   ┌──────────────────────────────────────────┐                                       │
│   │  VFS Storage                             │                                       │
│   │  ~/.mimiclaw/workspace/                  │                                       │
│   │  /spiffs/ (ESP32)                        │                                       │
│   │  config/, memory/, sessions/, skills/    │                                       │
│   └──────────────────────────────────────────┘                                       │
└─────────────────────────────────────────────────────────────────────────────────────┘
                             │
                             │  LLM API (HTTPS)
                             │  + Search API (HTTPS)
                             ▼
                    ┌───────────┐   ┌──────────────┐
                    │ LLM API   │   │ Search API   │
                    │(Claude/etc)│   │ (Brave/etc)  │
                    └───────────┘   └──────────────┘
```

---

## Message Bus Layer

The Message Bus serves as the central communication backbone between the Channel Layer and Core System, providing a unified message passing mechanism.

### Architecture Position

```
┌─────────────────────────────────────────────────────────────────┐
│                      Message Bus Layer                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────────────┐     ┌─────────────────────┐           │
│  │   Inbound Queue     │     │   Outbound Queue    │           │
│  │  (Channel→Agent)    │     │  (Agent→Channel)    │           │
│  │                     │     │                     │           │
│  │  • User messages    │     │  • Agent responses  │           │
│  │  • Commands         │     │  • Tool results     │           │
│  │  • Confirmation     │     │  • Confirm requests │           │
│  │    responses        │     │                     │           │
│  └──────────┬──────────┘     └──────────┬──────────┘           │
│             │                           │                      │
│             └───────────┬───────────────┘                      │
│                         │                                      │
│              ┌──────────▼──────────┐                           │
│              │    Message Bus      │                           │
│              │   (mimi_msg_t)      │                           │
│              │                     │                           │
│              │  Unified message    │                           │
│              │  format for all     │                           │
│              │  channels           │                           │
│              └─────────────────────┘                           │
└─────────────────────────────────────────────────────────────────┘
```

### Key Components

#### 1. Message Structure (`mimi_msg_t`)

```c
typedef enum {
    MIMI_MSG_TYPE_TEXT = 0,         /* Normal text message */
    MIMI_MSG_TYPE_CONTROL,          /* Control message (generic) */
    MIMI_MSG_TYPE_TOOL_RESULT,      /* Tool execution result */
} mimi_msg_type_t;

typedef enum {
    MIMI_CONTROL_TYPE_CONFIRM = 0,  /* Confirmation request */
    MIMI_CONTROL_TYPE_CANCEL,       /* Cancel operation */
    MIMI_CONTROL_TYPE_STOP,         /* Stop operation */
    MIMI_CONTROL_TYPE_STATUS,       /* Status query */
} mimi_control_type_t;

typedef struct {
    char channel[16];               /* "telegram", "websocket", "cli" */
    char chat_id[128];              /* Telegram/Feishu chat_id or WS client id */
    char *content;                  /* Heap-allocated message text (caller must free) */
    mimi_msg_type_t type;           /* Message type */
    
    /* Control message specific fields */
    mimi_control_type_t control_type;   /* Control type (for CONTROL messages) */
    char request_id[64];                /* Unique request ID */
    char target[64];                    /* Target (e.g., tool name, operation ID) */
    char data[1024];                    /* Additional data (e.g., tool params) */
} mimi_msg_t;
```

#### 2. Dual Queue System

| Queue | Direction | Purpose | API |
|-------|-----------|---------|-----|
| **Inbound** | Channel → Agent | User messages, commands, confirmation responses | `message_bus_push_inbound()` / `message_bus_pop_inbound()` |
| **Outbound** | Agent → Channel | Agent responses, tool results, confirmation requests | `message_bus_push_outbound()` / `message_bus_pop_outbound()` |

#### 3. Message Flow

**Inbound Flow (User → Agent):**
```
Channel (Telegram/CLI/etc)
    ↓
Channel Manager
    ↓
message_bus_push_inbound()
    ↓
Inbound Queue
    ↓
Agent Async Loop (message_bus_pop_inbound)
    ↓
LLM Processing / Tool Execution
```

**Outbound Flow (Agent → User):**
```
Agent Async Loop
    ↓
message_bus_push_outbound()
    ↓
Outbound Queue
    ↓
Outbound Dispatch Task (app.c)
    ↓
channel_send() → Channel Manager → Specific Channel
```

### Design Rationale

1. **Decoupling**: Channels and Agent are completely decoupled through the Message Bus
2. **Unified Format**: Single message format works across all channel types
3. **Async Communication**: Non-blocking message passing with queue-based buffering
4. **Thread Safety**: Queue operations are thread-safe, allowing concurrent access
5. **Extensibility**: Easy to add new message types (e.g., confirmation requests)

### Tool Confirmation Integration

For tool confirmation mechanism, the Message Bus plays a crucial role:

1. **Confirmation Request**: Agent sends confirmation request via `message_bus_push_outbound()`
2. **User Response**: Channel receives user response and routes via `message_bus_push_inbound()`
3. **State Management**: Agent maintains confirmation state while waiting for response

This design allows the confirmation mechanism to work seamlessly across all supported channels without channel-specific code.

---

## OS Abstraction Layer

The OS abstraction layer provides a unified interface for different OS backends, enabling seamless portability across platforms:

### Key Components

- **mimi_os_init()**: Initialize OS backend
- **mimi_os_get_version()**: Get OS backend version
- **mimi_os_start_scheduler()**: Start OS scheduler and run application
  - **POSIX**: Directly calls the provided function
  - **FreeRTOS**: Creates a task, starts scheduler, and runs the function in task context
- **Task Management**: `mimi_task_create()`, `mimi_task_delete()`
- **Synchronization**: `mimi_mutex_*()`, `mimi_cond_*()`
- **Timers**: `mimi_timer_start()`, `mimi_timer_stop()`
- **Time Functions**: `mimi_time_ms()`, `mimi_sleep_ms()`

### Architecture Benefits

- **Unified Main Entry**: Single `main.c` for all platforms
- **Easy Portability**: New RTOS backends only require implementing `os_xxx.c`
- **Consistent API**: Same interface across all platforms
- **Minimal Platform-Specific Code**: OS differences isolated in backend implementations

## Event-Driven Architecture

### Overview

The platform layer implements an event-driven architecture that separates I/O handling from business logic processing. This ensures the event loop thread never blocks on heavy operations.

### Core Components

#### 1. Event Loop Thread (`core/platform/runtime.c`)

The single event loop thread handles all I/O operations:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Event Loop Thread                             │
│                                                                  │
│  while (!exit) {                                                 │
│      mg_mgr_poll(10ms);        // Poll all connections           │
│      process_send_queue();     // Process pending sends          │
│  }                                                               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 2. Worker Thread Pool (`core/platform/event/event_dispatcher.c`)

Worker threads process business logic:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Worker Thread                                 │
│                                                                  │
│  while (running) {                                               │
│      msg = queue_recv(recv_queue);  // Block until message       │
│      handler(&msg);                  // Process business logic   │
│  }                                                               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

#### 3. Dual Queue System (`core/platform/event/event_bus.c`)

Two message queues bridge the event loop and worker threads:

| Queue | Direction | Purpose |
|-------|-----------|---------|
| `recv_queue` | Event Loop → Workers | Deliver received data to business logic |
| `send_queue` | Workers → Event Loop | Send data from any thread safely |

### Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         EVENT LOOP THREAD                                    │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ mg_mgr_poll()                                                          │  │
│  │     │                                                                  │  │
│  │     ├──▶ HTTP Response ──▶ http_ev_handler()/http_async_ev()          │  │
│  │     │                         │                                        │  │
│  │     │                         └──▶ event_bus_post_recv() (internal)   │  │
│  │     │                                                                  │  │
│  │     ├──▶ WS Message ──▶ ws_event_handler()                            │  │
│  │     │                         │                                        │  │
│  │     │                         ▼                                        │  │
│  │     │                   io_buf_from_const(data)                        │  │
│  │     │                         │                                        │  │
│  │     │                         ▼                                        │  │
│  │     │                   event_bus_post_recv() ──────▶ recv_queue       │  │
│  │     │                                                  (non-blocking)  │  │
│  │     │                                                                  │  │
│  │     └──▶ Connection Events (connect/disconnect/error)                 │  │
│  │                                                                        │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ process_send_queue()                                                   │  │
│  │     │                                                                  │  │
│  │     ▼                                                                  │  │
│  │   while (try_recv(send_queue, &msg)) {                                 │  │
│  │       switch (msg.type) {                                              │  │
│  │           case EVENT_MSG_SEND:                                         │  │
│  │               mg_ws_send(conn, buf);  // Actual send                   │  │
│  │               io_buf_unref(buf);                                       │  │
│  │               break;                                                   │  │
│  │           case EVENT_MSG_CLOSE:                                        │  │
│  │               mg_close_conn(conn);                                     │  │
│  │               break;                                                   │  │
│  │       }                                                                │  │
│  │   }                                                                    │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
                    │                                       ▲
                    ▼                                       │
           ┌──────────────┐                        ┌──────────────┐
           │  recv_queue  │                        │  send_queue  │
           └──────────────┘                        └──────────────┘
                    │                                       ▲
                    ▼                                       │
┌─────────────────────────────────────────────────────────────────────────────┐
│                         WORKER THREAD                                        │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ queue_recv(recv_queue, &msg, timeout)   // Blocking wait              │  │
│  │     │                                                                  │  │
│  │     ▼                                                                  │  │
│  │ handler = find_handler(msg.conn_type)                                  │  │
│  │     │                                                                  │  │
│  │     ▼                                                                  │  │
│  │ handler(&msg, user_data)               // Business logic              │  │
│  │     │                                                                  │  │
│  │     ├──▶ Parse JSON                                                    │  │
│  │     ├──▶ Call LLM API                                                  │  │
│  │     ├──▶ Execute tools                                                 │  │
│  │     │                                                                  │  │
│  │     ▼                                                                  │  │
│  │ io_buf_unref(msg.buf)                  // Cleanup                     │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ To send response:                                                      │  │
│  │                                                                        │  │
│  │   buf = io_buf_from_const(data)                                        │  │
│  │   event_bus_post_send(conn_id, buf) ──────▶ send_queue                │  │
│  │   io_buf_unref(buf)                                                    │  │
│  │                                                                        │  │
│  │ Returns immediately - actual send happens in event loop                │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Runtime / Dispatcher / Gateway / Channel Responsibilities

The current implementation follows a clear separation of concerns between I/O, scheduling, and business orchestration:

- **runtime (`core/platform/runtime.c`, single I/O thread)**  
  - Starts first in `app_start()` via `mimi_runtime_start()`.  
  - Owns the single event loop thread that runs `mg_mgr_poll()` in a loop.  
  - All socket operations (connect/accept/read/write/TLS/DNS/close) are driven from this thread, typically via Mongoose callbacks and `event_bus_post_*` calls.  
  - Other threads never touch raw `mg_connection` state directly; they only communicate through the event bus.

- **event bus + dispatcher (`core/platform/event/event_bus.c`, `core/platform/event/event_dispatcher.c`)**  
  - Created during runtime initialization; dispatcher workers are started from `runtime_start()`.  
  - Workers pull messages from `recv_queue` and execute short, non-blocking handlers (JSON parsing, routing, LLM calls, etc.).  
  - Long synchronous waits on network (e.g. 30s HTTP) are avoided here; instead, workers initiate async operations and react to completion events.

- **gateway layer (I/O adaptation)**  
  - Started from `gateway_system_start()` and registered via `gateway_manager`.  
  - Bridges between business intent and I/O:
    - Outbound: channels call gateway APIs, which translate into `event_bus_post_send()` / `event_bus_post_close()` so that runtime performs the actual socket I/O.  
    - Inbound: Mongoose callbacks in the platform layer post `EVENT_RECV` / `EVENT_CONNECT` / `EVENT_DISCONNECT` into the event bus; gateways register dispatcher handlers (e.g. `CONN_WS_SERVER`, `CONN_WS_CLIENT`, `CONN_HTTP_CLIENT`) that convert those into higher-level callbacks for channels.  
  - Gateways do not create their own threads; they are pure adapters between runtime and dispatcher.

- **channel layer (business orchestration / state machines)**  
  - Channels are started from `channel_system_start()` / `channel_start_all()` and call gateway APIs to configure and start transports.  
  - Complex startup flows (e.g. Feishu: tenant token → endpoints → configure WS → start WS) are implemented as asynchronous state machines:
    - `channel_start()` triggers the first async step and returns quickly on the main thread.  
    - HTTP/WS completions arrive as events on dispatcher workers, which advance the channel state (next HTTP call, WebSocket connect, retry, or fallback).  
  - Channels no longer perform "start + block until HTTP returns" patterns that could freeze the main thread; all long network waits are expressed as "fire request → handle completion event".

Under this model the boundaries are:

- **runtime**: only I/O.  
- **dispatcher**: only business handlers and state machine steps.  
- **gateway**: only event bridging (socket ↔ event bus ↔ channel callbacks).  
- **channel**: only business flow and state management.

### Zero-Copy Buffer (`io_buf.h`)

Reference-counted buffer for efficient data transfer:

```c
typedef struct io_buf {
    uint8_t *base;          // Data pointer (named for libuv compatibility)
    size_t len;             // Data length
    volatile int refcount;  // Reference count
    size_t capacity;        // Allocated capacity
} io_buf_t;

// Usage pattern:
io_buf_t *buf = io_buf_from_const(data, len);  // refcount = 1
event_bus_post_recv(..., buf);                  // queue holds reference
io_buf_unref(buf);                              // release our reference
// Buffer freed when last reference is released
```

### HTTP Integration

### LLM Proxy (services/llm/llm_proxy.c)

The LLM proxy handles communication with various LLM providers (OpenAI, Anthropic, OpenRouter).

#### Implementation (Asynchronous)

- **Asynchronous HTTP Requests**: Uses `mimi_http_exec_async()` for non-blocking HTTP calls
- **Callback-Based**: Uses callback functions to handle LLM responses
- **Benefits**:
  - **Concurrent Processing**: Multiple channels can make LLM requests simultaneously
  - **Non-Blocking**: Worker threads are not blocked, can handle other tasks
  - **Better Resource Utilization**: More efficient use of system resources
  - **Improved Responsiveness**: System remains responsive during LLM processing

#### API Interface

```c
/* Synchronous API (for simple use cases) */
mimi_err_t llm_chat_tools(const char *system_prompt,
                          cJSON *messages,
                          const char *tools_json,
                          llm_response_t *resp);

/* Asynchronous API (recommended for production) */
mimi_err_t llm_chat_tools_async(const char *system_prompt,
                               cJSON *messages,
                               const char *tools_json,
                               llm_response_t *resp,
                               llm_callback_t callback,
                               void *user_data);
```

#### Execution Flow (Asynchronous)

1. **Channel**: Receives user message and calls `llm_request_async()`
2. **LLM Proxy**: Builds JSON payload and calls `mimi_http_exec_async()`
3. **HTTP Module**: Sends request and returns immediately
4. **Worker Thread**: Freed to handle other tasks
5. **HTTP Response**: Received in event loop, posted to event bus
6. **Worker Thread**: Processes response and calls callback
7. **Channel**: Receives LLM response via callback and sends to user

### HTTP Integration

HTTP requests use the shared event loop with condition variable synchronization:

```
┌─────────────────────────────────────────────────────────────────┐
│  Calling Thread (e.g., Worker)                                   │
│                                                                  │
│  mimi_http_exec(req, resp)                                       │
│      │                                                           │
│      ├──▶ Create mutex + condition variable                      │
│      ├──▶ mg_http_connect(mgr, url, handler, ctx)                │
│      │        │                                                  │
│      │        └── Connection added to shared event loop          │
│      │                                                           │
│      ├──▶ cond_wait(ctx.cond, ctx.mutex, timeout)  // Block      │
│      │                                                           │
│      └──▶ Return when signaled by event loop                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                    │
                    ▼ (connection added to shared mgr)
┌─────────────────────────────────────────────────────────────────┐
│  Event Loop Thread                                               │
│                                                                  │
│  mg_mgr_poll()                                                   │
│      │                                                           │
│      ├──▶ HTTP response received                                 │
│      │                                                           │
│      └──▶ http_ev_handler()                                      │
│              │                                                   │
│              ├──▶ Copy response body                             │
│              │                                                   │
│              └──▶ cond_signal(ctx.cond)  // Wake caller          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Key Design Principles

| Principle | Implementation |
|-----------|----------------|
| **Non-blocking I/O** | Event loop uses 10ms poll timeout |
| **Thread safety** | All mongoose operations in event loop thread |
| **Zero-copy** | `io_buf` with reference counting |
| **Separation of concerns** | I/O in event loop, business logic in workers |
| **Backward compatible** | HTTP API remains synchronous for callers |

---

## Layered Architecture

### 1. Platform Layer (`main/core/platform/`)

The foundation layer providing OS abstraction and basic services.

**Components:**
- **OS Abstraction** (`os/`): Task/thread management, mutexes, condition variables, sleep functions
- **Runtime** (`runtime.c/h`): Event loop management, cleanup callbacks
- **Event System** (`event/event_bus.c/h`, `event/event_dispatcher.c/h`): Message queues and worker pool
- **I/O Buffer** (`io_buf.c/h`): Reference-counted zero-copy buffers
- **Logging** (`log.h`): Unified logging interface
- **Time** (`mimi_time.h`): Time and delay functions
- **File System** (`fs/`): VFS and direct file system operations

**Key Design:**
- Platform-agnostic APIs that work on both POSIX and FreeRTOS
- No business logic dependencies
- Event-driven I/O with worker thread pool
- Provides services via callback registration

### 2. Gateway Layer (`main/gateway/`)

Transport protocol abstraction layer. Each gateway handles a specific communication protocol.

**Components:**
- **Gateway Interface** (`gateway.h`): Unified gateway abstraction
- **Gateway Manager** (`gateway_manager.c/h`): Lifecycle management and registry
- **STDIO Gateway** (`stdio/`): Standard input/output transport
- **HTTP Gateway** (`http/`): HTTP client for REST APIs
- **WebSocket Gateway** (`websocket/`): WebSocket server and client

**Key Design:**
- Gateways use shared event loop (no separate threads for I/O)
- Events posted to queues for worker thread processing
- Thread-safe send operations via send queue
- Gateways are protocol-agnostic and don't know about business logic

**Gateway Interface:**
```c
typedef struct gateway {
    char name[32];
    gateway_type_t type;
    
    /* Lifecycle */
    mimi_err_t (*init)(gateway_t *gw, const gateway_config_t *cfg);
    mimi_err_t (*start)(gateway_t *gw);
    mimi_err_t (*stop)(gateway_t *gw);
    void (*destroy)(gateway_t *gw);
    
    /* Messaging */
    mimi_err_t (*send)(gateway_t *gw, const char *session_id, const char *content);
    
    /* Callbacks */
    void (*set_on_message)(gateway_t *gw, gateway_on_message_cb_t cb, void *user_data);
    void (*set_on_connect)(gateway_t *gw, gateway_on_connect_cb_t cb, void *user_data);
    void (*set_on_disconnect)(gateway_t *gw, gateway_on_disconnect_cb_t cb, void *user_data);
    
    bool is_initialized;
    bool is_started;
    void *priv_data;
} gateway_t;
```

### 3. Channel Layer (`main/channels/`)

Business logic layer handling protocol-specific message processing.

**Components:**
- **Channel Interface** (`channel.h`): Unified channel abstraction
- **Channel Manager** (`channel_manager.c/h`): Lifecycle management
- **CLI Channel** (`cli/`): Command-line interface channel
- **Telegram Channel** (`telegram/`): Telegram Bot API integration
- **Feishu Channel** (`feishu/`): Feishu/Lark integration
- **WebSocket Channel** (`websocket/`): WebSocket client channel
- **QQ Channel** (`qq/`): QQ Bot integration

**Key Design:**
- Each channel uses one or more gateways for transport
- Channels register event handlers with dispatcher
- Channels handle protocol-specific message formatting
- Business logic runs in worker threads
- **Control Message Support**: Channels support control messages for tool confirmation and other interactive operations

**Channel Interface (with Control Support):**
```c
struct channel {
    /* Basic lifecycle and messaging */
    mimi_err_t (*init)(channel_t *ch, const channel_config_t *cfg);
    mimi_err_t (*start)(channel_t *ch);
    mimi_err_t (*stop)(channel_t *ch);
    mimi_err_t (*send)(channel_t *ch, const char *session_id, const char *content);
    
    /* Control message handling */
    mimi_err_t (*send_control)(channel_t *ch, const char *session_id,
                               mimi_control_type_t control_type,
                               const char *request_id,
                               const char *target,
                               const char *data);
    void (*set_on_control_response)(channel_t *ch,
                                    void (*cb)(channel_t *, const char *session_id,
                                              const char *request_id,
                                              const char *response,
                                              void *user_data),
                                    void *user_data);
    /* ... other fields ... */
};
```

**Control Message Types:**
| Type | Purpose | Usage |
|------|---------|-------|
| `MIMI_CONTROL_TYPE_CONFIRM` | Tool execution confirmation | Request user approval before executing sensitive tools |
| `MIMI_CONTROL_TYPE_CANCEL` | Cancel operation | Cancel ongoing operations |
| `MIMI_CONTROL_TYPE_STOP` | Stop operation | Stop agent processing |
| `MIMI_CONTROL_TYPE_STATUS` | Status query | Query system or operation status |

**Control Message Flow:**
```
Agent Async Loop
    │
    │ 1. Detect tool requiring confirmation
    │ 2. Send control request via Message Bus
    ▼
Message Bus (Outbound Queue)
    │
    │ 3. Route to appropriate channel
    ▼
Channel (e.g., CLI/Telegram)
    │
    │ 4. Display confirmation prompt to user
    │ 5. Collect user response (ACCEPT/ACCEPT_ALWAYS/REJECT)
    ▼
Message Bus (Inbound Queue)
    │
    │ 6. Route control response to Agent
    ▼
Agent Async Loop
    │
    │ 7. Process confirmation result
    │ 8. Execute tool or abort based on response
```

**Channel-Gateway Relationship:**
```
Channel (Business Logic)
    │
    │ 1. Register handler with dispatcher
    │ 2. Configure gateway
    │ 3. Start gateway (uses shared event loop)
    │
    ▼
Event Loop (I/O)
    │
    │ 1. Receive data
    │ 2. Post to recv_queue
    │
    ▼
Worker Thread
    │
    │ 1. Call channel handler
    │ 2. Process business logic
    │ 3. Post send to send_queue
    │
    ▼
Event Loop
    │
    │ 1. Process send_queue
    │ 2. Actual network send
    │
    ▼
External
```

### 4. Command System (`main/interface/commands/`)

Shared command system used by all channels.

**Components:**
- **Command Registry** (`command_registry.c/h`): Command registration and execution
- **Command Implementations** (`cmd_*.c`): Individual command handlers

**Available Commands:**
| Command | Description | Example |
|---------|-------------|---------|
| `/help` | Show available commands | `/help` |
| `/session` | Session management | `/session list`, `/session new abc` |
| `/set` | Set configuration | `/set key value` |
| `/ask` | Ask AI a question | `/ask What is the weather?` |
| `/memory_read` | Read memory file | `/memory_read` |
| `/exit` | Exit application | `/exit` |

### 5. Core System

**Message Bus** (`main/bus/`):
- Two queues: Inbound (channels → agent) and Outbound (agent → channels)
- Message format: `mimi_msg_t` with channel, session_id, and content

**Agent Loop** (`main/agent/`):
- **Primary: Asynchronous Agent Loop** (`agent_async_loop.c`): Full-featured non-blocking agent with concurrent request support
- **Legacy: Synchronous Agent** (`agent_loop.c`): Simple blocking implementation for basic use cases

**Asynchronous Agent Loop** (`agent_async_loop.c`):
- **Non-blocking LLM Calls**: Uses `llm_chat_tools_async()` for asynchronous LLM requests
- **State Machine**: Tracks processing state through callbacks
- **Concurrent Processing**: Supports multiple concurrent requests (MAX_CONCURRENT=8)
- **Callback-Based**: Uses completion callbacks for LLM and tool responses
- **Asynchronous Tool Execution**: Tools run in worker thread pool, not blocking the main loop
- **Tool Worker Thread Pool**: 4 worker threads process tool executions in parallel

**Control Channel Mechanism**:

**Control Manager** (`main/agent/control/control_manager.c/h`):
- **Central Control Hub**: Manages control requests and responses
- **Request Tracking**: Tracks pending control requests by chat ID
- **Callback Management**: Handles asynchronous control responses
- **Thread Safe**: Uses mutex for thread synchronization

**Tool Call Context Manager** (`main/agent/tools/tool_call_context.c/h`):
- **Tool Execution State**: Manages tool execution state and confirmation status
- **Reference Counting**: Memory management for tool call contexts
- **Confirmation Tracking**: Tracks user confirmation status for tools
- **Always Allow List**: Maintains list of tools that don't require confirmation

**Control Flow**:
1. **Tool Use Detection**: Agent detects tool use in LLM response
2. **Confirmation Request**: Control Manager sends confirmation request to channel
3. **User Response**: Channel receives user confirmation (ACCEPT/ACCEPT_ALWAYS/REJECT)
4. **Response Handling**: Control Manager processes response and triggers callback
5. **Tool Execution**: Agent executes tool based on confirmation result

**Supported Control Types**:
- **CONFIRM**: Tool execution confirmation
- **CANCEL**: Operation cancellation
- **STOP**: Operation stopping
- **STATUS**: Status queries

**Channel Integration**:
- **CLI Channel**: Console-based confirmation prompts
- **Other Channels**: Channel-specific confirmation UI (Telegram buttons, etc.)

**Asynchronous State Management**:
- Control responses are processed asynchronously without blocking the main loop
- State is maintained through tool call contexts and control request tracking

**Detailed Tool Confirmation Flow**:

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                            工具确认流程数据流向                                     │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘

┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│   用户 (User)    │  │  CLI Channel    │  │ Control Manager │  │ Agent Async Loop │
└────────┬─────────┘  └────────┬─────────┘  └────────┬─────────┘  └────────┬─────────┘
         │                     │                     │                     │
         │ 1. 发送消息        │                     │                     │
         ├─────────────────────▶│                     │                     │
         │                     │                     │                     │
         │                     │ 2. 推送到入队     │                     │
         │                     ├─────────────────────▶│                     │
         │                     │                     │                     │
         │                     │                     │ 3. 处理消息       │
         │                     │                     ├─────────────────────▶│
         │                     │                     │                     │
         │                     │                     │                     │ 4. 调用LLM
         │                     │                     │                     ├──────────────────▶
         │                     │                     │                     │              LLM API
         │                     │                     │                     │◀──────────────────
         │                     │                     │                     │
         │                     │                     │                     │ 5. 检测工具调用
         │                     │                     │                     │
         │                     │                     │                     │ 6. 创建工具上下文
         │                     │                     │                     │
         │                     │                     │                     │ 7. 发送确认请求
         │                     │                     │◀─────────────────────┤
         │                     │                     │                     │
         │                     │ 8. 推送控制请求   │                     │
         │                     │◀─────────────────────┤                     │
         │                     │                     │                     │
         │ 9. 显示确认提示    │                     │                     │
         │◀─────────────────────┤                     │                     │
         │                     │                     │                     │
         │ 10. 选择确认选项   │                     │                     │
         ├─────────────────────▶│                     │                     │
         │                     │                     │                     │
         │                     │ 11. 推送控制响应  │                     │
         │                     ├─────────────────────▶│                     │
         │                     │                     │                     │
         │                     │                     │ 12. 查找待处理请求 │
         │                     │                     │                     │
         │                     │                     │ 13. 调用回调       │
         │                     │                     │◀─────────────────────┤
         │                     │                     │                     │
         │                     │                     │                     │ 14. 处理确认结果
         │                     │                     │                     │
         │                     │                     │                     │ 15. 执行工具
         │                     │                     │                     ├──────────────────▶
         │                     │                     │                     │              Tool Registry
         │                     │                     │                     │◀──────────────────
         │                     │                     │                     │
         │                     │                     │                     │ 16. 工具执行完成
         │                     │                     │                     │
         │                     │                     │                     │ 17. 发送结果
         │                     │                     │                     ├──────────────────▶
         │                     │                     │                     │              Message Bus
         │                     │                     │                     │◀──────────────────
         │                     │                     │                     │
         │ 18. 显示执行结果    │                     │                     │
         │◀─────────────────────┤                     │                     │
         │                     │                     │                     │
└──────────────────┘  └──────────────────┘  └──────────────────┘  └──────────────────┘
```

**Phase 1: User Request Processing**
1. **User sends message**: User inputs message through CLI terminal
2. **CLI Channel receives**: CLI Channel receives user message and pushes to inbound queue
3. **Agent processing**: Agent Async Loop retrieves message from inbound queue
4. **LLM call**: Agent calls LLM API to process user request

**Phase 2: Tool Call Detection**
5. **Detect tool call**: Agent detects tool call in LLM response
6. **Create tool context**: Agent creates tool call context with tool name, parameters, etc.
7. **Check confirmation need**: Agent checks if tool requires user confirmation
8. **Send confirmation request**: If confirmation needed, Agent sends confirmation request via Control Manager

**Phase 3: Confirmation Request Display**
9. **Push control request**: Control Manager pushes control request to outbound queue
10. **Display confirmation prompt**: CLI Channel retrieves control request from outbound queue and displays confirmation prompt
11. **User selection**: User selects confirmation option (1=ACCEPT, 2=ACCEPT_ALWAYS, 3=REJECT)

**Phase 4: Confirmation Response Processing**
12. **Push control response**: CLI Channel pushes user's selection as control response to inbound queue
13. **Find pending request**: Control Manager finds corresponding pending control request
14. **Invoke callback**: Control Manager invokes registered callback function to process confirmation result

**Phase 5: Tool Execution**
15. **Process confirmation result**: Agent decides whether to execute tool based on user's confirmation
16. **Execute tool**: If user confirmed, Agent executes tool via Tool Registry
17. **Tool execution complete**: Tool Registry completes tool execution and returns result

**Phase 6: Result Return**
18. **Send result**: Agent pushes tool execution result to outbound queue
19. **Display execution result**: CLI Channel retrieves result from outbound queue and displays to user

**File and Function Mapping**:

| Step | File | Function | Description |
|------|------|----------|-------------|
| 1-2 | `main/channels/cli/cli_channel.c` | `on_gateway_message()` | Receives user message from gateway |
| 2-3 | `main/core/bus/message_bus.c` | `message_bus_push_inbound()` | Pushes message to inbound queue |
| 3-4 | `main/agent/agent_async_loop.c` | `agent_async_loop_process_message()` | Processes inbound message |
| 4-5 | `main/agent/agent_async_loop.c` | `llm_chat_tools_async()` | Calls LLM API asynchronously |
| 5-6 | `main/agent/agent_async_loop.c` | `tool_call_context_create()` | Creates tool call context |
| 6-7 | `main/agent/agent_async_loop.c` | `tool_call_context_is_always_allowed()` | Checks if tool requires confirmation |
| 7-8 | `main/agent/agent_async_loop.c` | `control_manager_send_request()` | Sends confirmation request |
| 8-9 | `main/agent/control/control_manager.c` | `control_manager_send_request()` | Pushes control request to outbound queue |
| 9-10 | `main/app/app.c` | `outbound_dispatch_task()` | Processes outbound queue messages |
| 9-10 | `main/channels/cli/cli_channel.c` | `cli_channel_send_control()` | Displays confirmation prompt |
| 10-11 | `main/channels/cli/cli_channel.c` | `gateway_send()` | Sends prompt to user via gateway |
| 11-12 | `main/channels/cli/cli_channel.c` | `on_gateway_message()` | Receives user selection |
| 11-12 | `main/channels/cli/cli_channel.c` | `control_manager_handle_response_by_chat_id()` | Handles control response |
| 12-13 | `main/agent/control/control_manager.c` | `control_manager_handle_response_by_chat_id()` | Finds pending request |
| 13-14 | `main/agent/control/control_manager.c` | `control_manager_handle_response_by_chat_id()` | Invokes callback |
| 14-15 | `main/agent/agent_async_loop.c` | `tool_confirm_callback()` | Processes confirmation result |
| 15-16 | `main/agent/agent_async_loop.c` | `tool_confirm_execution_callback()` | Executes tool if confirmed |
| 15-16 | `main/agent/tools/tool_registry.c` | `tool_execute()` | Executes tool via registry |
| 16-17 | `main/agent/agent_async_loop.c` | `tool_confirm_execution_callback()` | Handles tool completion |
| 17-18 | `main/agent/agent_async_loop.c` | `message_bus_push_outbound()` | Pushes result to outbound queue |
| 18-19 | `main/app/app.c` | `outbound_dispatch_task()` | Processes outbound queue messages |
| 18-19 | `main/channels/cli/cli_channel.c` | `cli_channel_send_impl()` | Displays result to user |

**Data Flow Characteristics**:
1. **Asynchronous Processing**: All operations are asynchronous without blocking the main thread
2. **Message Bus**: Component decoupling through Message Bus
3. **Control Manager**: Unified management of all control requests and responses
4. **Tool Context**: Maintains complete state information of tool calls
5. **User Interaction**: Channel is responsible for user interaction interface

**Asynchronous Tool Execution Flow:**
```
┌─────────────────────────────────────────────────────────────────┐
│                     Async Agent Loop                             │
│                                                                  │
│  1. Receive message from inbound queue                          │
│  2. Start async LLM call (llm_chat_tools_async)                │
│  3. Return immediately, continue processing next message      │
│                                                                  │
│  LLM Response Received:                                         │
│  4. If tool_use detected:                                      │
│     - Queue tools to worker thread pool                        │
│     - Return immediately                                        │
│                                                                  │
│  All Tools Complete:                                            │
│  5. Append tool results to messages                            │
│  6. Start next LLM call                                        │
│                                                                  │
│  No Tool Use:                                                   │
│  7. Send response to outbound queue                            │
│  8. Mark request complete                                      │
└─────────────────────────────────────────────────────────────────┘
```

**Tool Worker Thread Pool:**
- 4 worker threads for parallel tool execution
- Non-blocking tool execution via callback
- Support for multiple concurrent tool calls
- Thread-safe queue with 16 slot capacity

**Tools** (`main/tools/`):
- Tool registry for dynamic tool registration
- Tool implementations (web_search, etc.)

**Memory** (`main/memory/`):
- Session management (JSONL files)
- Long-term memory (MEMORY.md)
- Daily notes

---

## Thread Model

### Thread Ownership

| Component | Thread Ownership | Notes |
|-----------|-----------------|-------|
| **Platform Layer** | Event Loop Thread | Single shared event loop |
| **Event Dispatcher** | Worker Pool (2 threads) | Process business logic |
| **Gateway Layer** | Uses shared event loop | No separate threads |
| **Channel Layer** | Uses worker pool | Handlers run in workers |
| **Agent Loop** | Owns one thread | Message processing |
| **Outbound Dispatch** | Owns one thread | Message routing |
| **Heartbeat** | Owns one thread | Periodic tasks |
| **Cron Service** | Owns one thread | Scheduled jobs |

### Thread Interaction

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  Event Loop     │     │  Worker Pool    │     │  Agent Loop     │
│  (runtime.c)    │     │  (dispatcher)   │     │  (agent_loop)   │
├─────────────────┤     ├─────────────────┤     ├─────────────────┤
│ mg_mgr_poll()   │────▶│ recv_queue      │     │                 │
│                 │     │                 │────▶│ inbound_queue   │
│ process_send()  │◀────│ send_queue      │     │                 │
│                 │     │                 │◀────│ outbound_queue  │
└─────────────────┘     └─────────────────┘     └─────────────────┘
```

---

## Directory Structure

```
main/
├── main.c                      # Entry point
├── mimi_config.h               # Compile-time configuration
├── mimi_secrets.h              # Build-time credentials (gitignored)
│
├── app/                        # Application Layer
│   └── app.c                   # Application main
│
├── core/                       # Core System
│   ├── platform/               # Platform Layer
│   │   ├── os/                 # OS Abstraction
│   │   │   ├── os.h            # OS interface (task, mutex, cond)
│   │   │   ├── posix_impl/     # POSIX implementation
│   │   │   └── freertos_impl/  # FreeRTOS implementation
│   │   ├── runtime.c/h         # Event loop management
│   │   ├── event/
│   │   │   ├── event_bus.c/h   # Event bus (cross-thread message transport)
│   │   │   ├── event_dispatcher.c/h # Worker thread pool
│   │   │   └── io_buf.c/h      # Reference-counted I/O buffer
│   │   ├── queue.c/h           # Thread-safe queue
│   │   ├── log.h               # Logging interface
│   │   ├── mimi_time.h         # Time functions
│   │   ├── mimi_err.h          # Error codes
│   │   ├── fs/                 # File system
│   │   │   ├── mimi_fs.h       # VFS interface
│   │   │   └── posix/          # POSIX implementation
│   │   ├── http/               # HTTP client
│   │   ├── websocket/          # WebSocket
│   │   └── path_utils.c/h      # Path utilities
│   ├── bus/                    # Message Bus
│   │   └── message_bus.c/h     # Inbound/outbound queues
│   └── config/                 # Configuration
│       ├── config.c/h          # Config loading
│       └── workspace_bootstrap.c # Workspace setup
│
├── services/                   # Service Layer
│   ├── llm/                    # LLM Integration
│   │   └── llm_proxy.c/h       # LLM API client
│   ├── cron/                   # Cron Service
│   │   └── cron_service.c/h    # Scheduled jobs
│   ├── heartbeat/              # Heartbeat Service
│   │   └── heartbeat.c/h       # Periodic heartbeat
│   └── proxy/                  # HTTP Proxy
│       └── http_proxy.c/h      # Proxy configuration
│
├── interface/                  # Interface Layer
│   ├── cli/                    # CLI Framework
│   │   ├── cli_terminal.c/h    # Terminal handling
│   │   └── editor.c/h          # Line editor
│   ├── commands/               # Command System
│   │   ├── command_registry.c/h # Command registry
│   │   ├── cmd_help.c          # /help command
│   │   ├── cmd_session.c       # /session command
│   │   ├── cmd_set.c           # /set command
│   │   ├── cmd_ask.c           # /ask command
│   │   ├── cmd_memory.c        # /memory_read command
│   │   └── cmd_exit.c          # /exit command
│   └── router/                 # Router Layer
│       └── router.c/h          # Message routing
│
├── agent/                      # Agent System
│   ├── agent_async.c/h         # Async agent functions
│   ├── agent_async_loop.c/h    # Main agent loop
│   ├── context_builder.c/h     # Context building
│   ├── control/                # Control Channel
│   │   └── control_manager.c/h # Control manager
│   └── tools/                  # Tool System
│       ├── tool_registry.c/h   # Tool registry
│       ├── tool_call_context.c/h # Tool call context management
│       ├── tool_web_search.c   # Web search tool
│       ├── tool_get_time.c     # Time tool
│       ├── tool_files.c        # File operations
│       └── tool_cron.c         # Cron tool
│
├── channels/                   # Channel Layer
│   ├── channel.h               # Channel interface
│   ├── channel_manager.c/h     # Channel registry
│   ├── cli/                    # CLI channel
│   ├── telegram/               # Telegram channel
│   ├── feishu/                 # Feishu channel
│   ├── websocket/              # WebSocket channel
│   └── qq/                     # QQ channel
│
├── gateway/                    # Gateway Layer
│   ├── gateway.h               # Gateway interface
│   ├── gateway_manager.c/h     # Gateway registry
│   ├── stdio/                  # STDIO transport
│   ├── http/                   # HTTP client
│   └── websocket/              # WebSocket server/client
│
├── memory/                     # Memory System
│   ├── memory_store.c/h        # Long-term memory
│   └── session_mgr.c/h         # Session management
│
├── skills/                     # Skills System
│   └── skill_loader.c/h        # Dynamic skill loading
│
└── wifi/                       # WiFi Manager (ESP32)
    └── wifi_manager.c/h        # WiFi management
```

---

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux | ✅ Supported | Primary development platform |
| macOS | ✅ Supported | POSIX compatible |
| ESP32 | 🔄 In Progress | FreeRTOS-based |
| Windows | 🔄 In Progress | Basic support |
