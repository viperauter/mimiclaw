# MimiClaw Architecture

> Cross-platform AI Agent framework — C implementation supporting both POSIX systems (Linux/macOS) and embedded platforms (ESP32).

---

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                              External Interfaces                                     │
│                                                                                      │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│   │   Telegram   │    │   WebSocket  │    │     CLI      │    │    Feishu    │      │
│   │     Bot      │    │    Server    │    │   Terminal   │    │     Bot      │      │
│   │  (HTTPS API) │    │  (WS Protocol)│    │   (STDIO)    │    │  (WS+HTTP)   │      │
│   └──────┬───────┘    └──────┬───────┘    └──────┬───────┘    └──────┬───────┘      │
└──────────┼──────────────────┼──────────────────┼──────────────────┼────────────────┘
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

## Layered Architecture

### 1. Platform Layer (`main/platform/`)

The foundation layer providing OS abstraction and basic services.

**Components:**
- **OS Abstraction** (`os/`): Task/thread management, mutexes, sleep functions
- **Runtime** (`runtime.c/h`): Event loop management, cleanup callbacks
- **Logging** (`log.h`): Unified logging interface
- **Time** (`mimi_time.h`): Time and delay functions
- **File System** (`fs/`): VFS and direct file system operations

**Key Design:**
- Platform-agnostic APIs that work on both POSIX and FreeRTOS
- No business logic dependencies
- Provides services via callback registration (e.g., cleanup callbacks)

### 2. Gateway Layer (`main/gateway/`)

Transport protocol abstraction layer. Each gateway handles a specific communication protocol.

**Components:**
- **Gateway Interface** (`gateway.h`): Unified gateway abstraction
- **Gateway Manager** (`gateway_manager.c/h`): Lifecycle management and registry
- **STDIO Gateway** (`stdio/`): Standard input/output transport
- **HTTP Gateway** (`http/`): HTTP client for REST APIs
- **WebSocket Gateway** (`websocket/`): WebSocket server and client

**Key Design:**
- Each gateway owns its own thread(s) for I/O operations
- Thread creation happens in gateway's `start()` implementation
- Gateways are protocol-agnostic and don't know about business logic
- Channel layer uses gateways via the unified interface

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
- Channels register callbacks with gateways to receive messages
- Channels handle protocol-specific message formatting
- Thread management is delegated to gateways

**Channel-Gateway Relationship:**
```
Channel (Business Logic)
    │
    │ 1. Find gateway
    │ 2. Register callback
    │ 3. Call gateway_start()
    │
    ▼
Gateway (Transport)
    │
    │ 1. Create thread(s)
    │ 2. Handle I/O
    │ 3. Call channel callback
    │
    ▼
Channel (Process message)
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

## Data Flow

### Inbound Message Flow

```
1. External input received
   (Telegram HTTPS / WebSocket / STDIO / Feishu)
   
2. Gateway layer processes transport
   - Gateway thread receives raw data
   - Protocol-specific parsing
   
3. Gateway calls channel callback
   - on_message(gateway, session_id, content, channel_data)
   
4. Channel processes business logic
   - Parse commands or chat messages
   - Wrap in mimi_msg_t
   
5. Message pushed to Inbound Queue
   
6. Agent Loop processes message
   - Load session history
   - Build context (SOUL.md + USER.md + memory)
   - Call LLM API with ReAct loop
   - Push response to Outbound Queue
```

### Outbound Message Flow

```
7. Outbound Dispatch receives message
   - Routes by channel field
   
8. Channel sends via gateway
   - channel_send() → gateway_send()
   
9. Gateway transmits to external
   - HTTP POST / WebSocket frame / STDIO output
```

---

## Thread Model

### Thread Ownership

| Component | Thread Ownership | Notes |
|-----------|-----------------|-------|
| **Platform Layer** | None | Provides thread creation API only |
| **Gateway Layer** | Each gateway owns its thread(s) | Created in gateway_start() |
| **Channel Layer** | None | Uses gateway threads via callbacks |
| **Agent Loop** | Owns one thread | Message processing |
| **Outbound Dispatch** | Owns one thread | Message routing |
| **Heartbeat** | Owns one thread | Periodic tasks |
| **Cron Service** | Owns one thread | Scheduled jobs |

### Thread Creation Pattern

```c
// Gateway implementation example
static void gateway_task(void *arg)
{
    while (s_running) {
        // Poll for I/O
        poll_io();
        mimi_sleep_ms(100);
    }
}

mimi_err_t gateway_start(gateway_t *gw)
{
    // Initialize resources
    
    // Create thread for I/O
    s_running = true;
    return mimi_task_create_detached("gateway_name", gateway_task, NULL);
}

mimi_err_t gateway_stop(gateway_t *gw)
{
    // Signal thread to stop
    s_running = false;
    
    // Cleanup resources
}
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
│   │   ├── os.h                # OS interface
│   │   └── posix_impl/         # POSIX implementation
│   ├── runtime.c/h             # Runtime management
│   ├── log.h                   # Logging interface
│   ├── mimi_time.h             # Time functions
│   ├── mimi_err.h              # Error codes
│   └── fs/                     # File system
│       ├── mimi_fs.h           # VFS interface
│       └── posix/              # POSIX implementation
│
├── gateway/                    # Gateway Layer
│   ├── gateway.h               # Gateway interface
│   ├── gateway.c               # Gateway base implementation
│   ├── gateway_manager.c/h     # Gateway lifecycle management
│   ├── stdio/                  # STDIO Gateway
│   │   ├── stdio_gateway.c/h
│   │   └── stdio_transport.c/h
│   ├── http/                   # HTTP Gateway
│   │   └── http_gateway.c/h
│   └── websocket/              # WebSocket Gateway
│       ├── ws_gateway.c/h      # WebSocket Server
│       └── ws_client_gateway.c/h  # WebSocket Client
│
├── channels/                   # Channel Layer
│   ├── channel.h               # Channel interface
│   ├── channel_manager.c/h     # Channel lifecycle management
│   ├── cli/                    # CLI Channel
│   │   └── cli_channel.c/h
│   ├── telegram/               # Telegram Channel
│   │   └── telegram_channel.c/h
│   ├── feishu/                 # Feishu Channel
│   │   └── feishu_channel.c/h
│   ├── websocket/              # WebSocket Channel
│   │   └── ws_channel.c/h
│   └── qq/                     # QQ Channel
│       └── qq_channel.c/h
│
├── commands/                   # Command System
│   ├── command.h               # Command interface
│   ├── command_registry.c/h    # Command registration
│   ├── cmd_help.c              # /help command
│   ├── cmd_session.c           # /session command
│   ├── cmd_set.c               # /set command
│   ├── cmd_ask.c               # /ask command
│   ├── cmd_memory.c            # /memory_read command
│   └── cmd_exit.c              # /exit command
│
├── cli/                        # CLI Framework
│   ├── cli_terminal.c/h        # Terminal abstraction
│   ├── editor.c/h              # Line editor
│   └── cli.md                  # Documentation
│
├── bus/                        # Message Bus
│   ├── message_bus.h           # Message format
│   └── message_bus.c           # Queue implementation
│
├── agent/                      # Agent System
│   ├── agent_loop.c/h          # Main agent task
│   ├── context_builder.c/h     # Context building
│   └── agent.h                 # Agent interface
│
├── llm/                        # LLM Integration
│   ├── llm_proxy.c/h           # LLM API client
│   └── llm_provider.h          # Provider interface
│
├── tools/                      # Tool System
│   ├── tool_registry.c/h       # Tool registration
│   ├── tool_web_search.c/h     # Web search tool
│   └── tool_execute.c/h        # Tool execution
│
├── memory/                     # Memory System
│   ├── memory_store.c/h        # Long-term memory
│   ├── session_mgr.c/h         # Session management
│   └── memory.h                # Memory interface
│
├── router/                     # Message Router
│   └── router.c/h              # Message routing
│
├── heartbeat/                  # Heartbeat Service
│   └── heartbeat.c/h           # Periodic status updates
│
├── cron/                       # Cron Service
│   └── cron_service.c/h        # Scheduled tasks
│
├── skills/                     # Skills System
│   └── skills.c/h              # Skill management
│
├── config/                     # Configuration
│   └── config.c/h              # Runtime configuration
│
├── app/                        # Application Layer
│   └── app.c/h                 # Application initialization
│
└── utils/                      # Utilities
    ├── json_utils.c/h          # JSON helpers
    └── string_utils.c/h        # String helpers
```

---

## Key Design Principles

### 1. Layer Isolation

- **Platform Layer**: No dependencies on upper layers
- **Gateway Layer**: Only depends on Platform Layer
- **Channel Layer**: Depends on Gateway and Platform Layers
- **Application Layer**: Orchestrates all layers

### 2. Thread Ownership

- Each component that needs concurrent execution owns its thread(s)
- No shared threads between components
- Communication via queues and callbacks

### 3. Callback-Based Communication

- Gateways notify channels via callbacks
- Channels don't poll gateways
- Platform layer uses cleanup callbacks for business logic

### 4. Protocol Abstraction

- Gateway layer abstracts transport protocols
- Channel layer abstracts business logic
- Easy to add new protocols or channels

---

## Configuration

### Build-time Configuration (`mimi_secrets.h`)

| Define | Description |
|--------|-------------|
| `MIMI_SECRET_WIFI_SSID` | WiFi SSID (ESP32) |
| `MIMI_SECRET_WIFI_PASS` | WiFi password (ESP32) |
| `MIMI_SECRET_API_KEY` | LLM API key |
| `MIMI_SECRET_MODEL` | Model ID |

### Runtime Configuration (`~/.mimiclaw/config.json`)

```json
{
  "agents": {
    "defaults": {
      "model": "claude-3-opus-20240229",
      "provider": "anthropic",
      "maxTokens": 4096
    }
  },
  "channels": {
    "telegram": {
      "enabled": true,
      "token": "YOUR_TOKEN"
    }
  },
  "providers": {
    "anthropic": {
      "apiKey": "YOUR_KEY",
      "apiBase": "https://api.anthropic.com/v1"
    }
  }
}
```

---

## Storage Layout (VFS)

```
~/.mimiclaw/
├── config.json                 # Runtime configuration (real path)
└── workspace/                  # VFS base directory
    ├── config/
    │   ├── SOUL.md            # AI personality
    │   └── USER.md            # User profile
    ├── memory/
    │   ├── MEMORY.md          # Long-term memory
    │   └── daily/             # Daily notes
    │       └── 2024-01-01.md
    ├── sessions/              # Session files
    │   ├── cli_default.jsonl
    │   ├── tg_12345.jsonl
    │   └── ws_client1.jsonl
    └── skills/                # Skill files
        └── weather.md
```

---

## Startup Sequence

### Initialization Phase (app_init)

```
app_init()
  ├── mimi_fs_init()              # Initialize VFS
  ├── posix_fs_register()         # Register POSIX filesystem
  ├── mimi_workspace_bootstrap()  # Setup workspace
  ├── mimi_kv_init()              # Initialize KV store
  ├── http_proxy_init()           # Initialize HTTP proxy
  ├── message_bus_init()          # Create inbound/outbound queues
  ├── mimi_runtime_init()         # Initialize runtime
  ├── command_system_auto_init()  # Initialize command system
  ├── gateway_system_init()       # Initialize gateway system
  │   ├── gateway_manager_init()  # Init gateway manager
  │   ├── Register all gateways   # STDIO, WS, HTTP, WS Client
  │   └── Initialize gateways     # Call gateway_init() for each
  ├── channel_system_init()       # Initialize channel system
  │   ├── router_init()           # Initialize message router
  │   ├── channel_manager_init()  # Init channel manager
  │   └── Register all channels   # CLI, Telegram, Feishu, QQ, WS
  ├── memory_store_init()         # Initialize memory system
  ├── skill_loader_init()         # Initialize skill loader
  ├── session_mgr_init()          # Initialize session manager
  ├── tool_registry_init()        # Initialize tool registry
  ├── llm_proxy_init()            # Initialize LLM proxy
  └── agent_loop_init()           # Initialize agent loop
```

### Startup Phase (app_start)

```
app_start()
  ├── agent_loop_start()          # Start agent processing
  ├── Create outbound_dispatch task  # Message routing task
  ├── cron_service_init/start()   # Start cron service
  ├── heartbeat_init/start()      # Start heartbeat service
  ├── Register cleanup callbacks  # Register app_cleanup()
  ├── gateway_system_start()      # Start all gateways
  │   └── Each gateway creates its thread(s)
  ├── channel_system_start()      # Start all channels
  │   └── Each channel starts its gateway(s)
  └── Application ready
```

### Runtime Phase (app_run)

```
app_run()
  └── mimi_runtime_run()          # Main event loop
      └── Cleanup on exit
          ├── Execute cleanup callbacks
          ├── Stop all channels
          └── Stop all gateways
```

### Key Functions

| Function | Phase | Description |
|----------|-------|-------------|
| `gateway_system_init()` | Init | Register and initialize all gateways |
| `gateway_system_start()` | Start | Start all gateways (create threads) |
| `gateway_system_stop()` | Stop | Stop all gateways |
| `channel_system_init()` | Init | Register and initialize all channels |
| `channel_system_start()` | Start | Start all channels |
| `channel_system_stop()` | Stop | Stop all channels |

---

## Migration Status

| Component | Status | Location |
|-----------|--------|----------|
| Platform Layer | ✅ Completed | `platform/` |
| Gateway Layer | ✅ Completed | `gateway/` |
| Channel Layer | ✅ Completed | `channels/` |
| Command System | ✅ Completed | `commands/` |
| Message Bus | ✅ Completed | `bus/` |
| Agent System | ✅ Completed | `agent/` |
| Tool System | ✅ Completed | `tools/` |
| Memory System | ✅ Completed | `memory/` |
| VFS Layer | ✅ Completed | `platform/fs/` |

---

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| Linux (POSIX) | ✅ Supported | Full feature set |
| macOS (POSIX) | ✅ Supported | Full feature set |
| ESP32 (FreeRTOS) | 🔄 In Progress | WiFi, OTA, SPIFFS |
| Windows | 🔄 In Progress | Basic support |
