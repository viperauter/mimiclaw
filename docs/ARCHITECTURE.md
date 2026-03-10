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
│                              Core System                                             │
│                                                                                      │
│   ┌─────────────┐       ┌──────────────────┐                                        │
│   │  Inbound    │◀──────│   Message Bus    │                                        │
│   │   Queue     │       │   (mimi_msg_t)   │                                        │
│   └──────┬──────┘       └──────────────────┘                                        │
│          │                                                                           │
│          ▼                                                                           │
│   ┌────────────────────────┐                                                        │
│   │     Agent Loop          │                                                       │
│   │                         │                                                       │
│   │  Context ──▶ LLM Proxy │                                                       │
│   │  Builder      (HTTPS)   │                                                       │
│   │       ▲          │      │                                                       │
│   │       │     tool_use?   │                                                       │
│   │       │          ▼      │                                                       │
│   │  Tool Results ◀─ Tools  │                                                       │
│   │              (web_search)│                                                      │
│   └──────────┬─────────────┘                                                        │
│              │                                                                       │
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

## Event-Driven Architecture

### Overview

The platform layer implements an event-driven architecture that separates I/O handling from business logic processing. This ensures the event loop thread never blocks on heavy operations.

### Core Components

#### 1. Event Loop Thread (`runtime.c`)

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

#### 2. Worker Thread Pool (`event/event_dispatcher.c`)

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

#### 3. Dual Queue System (`event/event_bus.c`)

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
│  │     ├──▶ HTTP Response ──▶ http_ev_handler() ──▶ Signal cond_wait     │  │
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

### 1. Platform Layer (`main/platform/`)

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

### 4. Command System (`main/commands/`)

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
- Processes messages from inbound queue
- Builds context and calls LLM API
- Handles tool use and ReAct loop
- Sends responses to outbound queue

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
├── platform/                   # Platform Layer
│   ├── os/                     # OS Abstraction
│   │   ├── os.h                # OS interface (task, mutex, cond)
│   │   └── posix_impl/         # POSIX implementation
│   ├── runtime.c/h             # Event loop management
│   ├── event/
│   │   ├── event_bus.c/h       # Event bus (cross-thread message transport)
│   │   ├── event_dispatcher.c/h # Worker thread pool
│   │   └── io_buf.c/h          # Reference-counted I/O buffer
│   ├── queue.c/h               # Thread-safe queue
│   ├── log.h                   # Logging interface
│   ├── mimi_time.h             # Time functions
│   ├── mimi_err.h              # Error codes
│   └── fs/                     # File system
│       ├── mimi_fs.h           # VFS interface
│       └── posix/              # POSIX implementation
│
├── gateway/                    # Gateway Layer
│   ├── gateway.h               # Gateway interface
│   ├── gateway_manager.c/h     # Gateway registry
│   ├── stdio/                  # STDIO transport
│   ├── http/                   # HTTP client
│   └── websocket/              # WebSocket server/client
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
├── commands/                   # Command System
│   ├── command_registry.c/h    # Command registry
│   ├── cmd_help.c              # /help command
│   ├── cmd_session.c           # /session command
│   ├── cmd_set.c               # /set command
│   ├── cmd_ask.c               # /ask command
│   ├── cmd_memory.c            # /memory_read command
│   └── cmd_exit.c              # /exit command
│
├── bus/                        # Message Bus
│   └── message_bus.c/h         # Inbound/outbound queues
│
├── agent/                      # Agent System
│   ├── agent_loop.c/h          # Main agent loop
│   └── context_builder.c/h     # Context building
│
├── llm/                        # LLM Integration
│   └── llm_proxy.c/h           # LLM API client
│
├── tools/                      # Tool System
│   ├── tool_registry.c/h       # Tool registry
│   ├── tool_web_search.c       # Web search tool
│   ├── tool_get_time.c         # Time tool
│   ├── tool_files.c            # File operations
│   └── tool_cron.c             # Cron tool
│
├── memory/                     # Memory System
│   ├── memory_store.c/h        # Long-term memory
│   └── session_mgr.c/h         # Session management
│
├── skills/                     # Skills System
│   └── skill_loader.c/h        # Dynamic skill loading
│
├── router/                     # Router Layer
│   └── router.c/h              # Message routing
│
├── config/                     # Configuration
│   ├── config.c/h              # Config loading
│   └── workspace_bootstrap.c   # Workspace setup
│
├── cli/                        # CLI Framework
│   ├── cli_terminal.c/h        # Terminal handling
│   └── editor.c/h              # Line editor
│
├── cron/                       # Cron Service
│   └── cron_service.c/h        # Scheduled jobs
│
├── heartbeat/                  # Heartbeat Service
│   └── heartbeat.c/h           # Periodic heartbeat
│
├── proxy/                      # HTTP Proxy
│   └── http_proxy.c/h          # Proxy configuration
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
