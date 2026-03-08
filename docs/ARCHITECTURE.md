# MimiClaw Architecture

> ESP32-S3 AI Agent firmware — C/FreeRTOS implementation running on bare metal (no Linux).

---

## System Overview

```
Telegram App (User)          WebSocket Client              CLI Terminal
    │                              │                            │
    │  HTTPS Long Polling          │  WS Protocol               │  STDIO
    │                              │                            │
    ▼                              ▼                            ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Unified Channel Layer                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │   Telegram   │  │  WebSocket   │  │     CLI      │  │   (Future)   │     │
│  │   Channel    │  │   Channel    │  │   Channel    │  │              │     │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────────────┘     │
│         │                  │                  │                              │
│         └──────────────────┴──────────────────┘                              │
│                            │                                                 │
│              ┌─────────────▼──────────────┐                                  │
│              │     Channel Manager        │                                  │
│              │  (Lifecycle & Routing)     │                                  │
│              └─────────────┬──────────────┘                                  │
└────────────────────────────┼────────────────────────────────────────────────┘
                             │
                             ▼
              ┌──────────────────────────────┐
              │    Shared Command System     │
              │  /help /session /set /ask    │
              │  /memory_read /exit          │
              └──────────────┬───────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                            Core System                                       │
│                                                                              │
│   ┌─────────────┐       ┌──────────────────┐                                 │
│   │  Inbound    │◀──────│   Message Bus    │                                 │
│   │   Queue     │       │   (mimi_msg_t)   │                                 │
│   └──────┬──────┘       └──────────────────┘                                 │
│          │                                                                   │
│          ▼                                                                   │
│   ┌────────────────────────┐                                                 │
│   │     Agent Loop          │                                                │
│   │     (Core 1)            │                                                │
│   │                         │                                                │
│   │  Context ──▶ LLM Proxy │                                                │
│   │  Builder      (HTTPS)   │                                                │
│   │       ▲          │      │                                                │
│   │       │     tool_use?   │                                                │
│   │       │          ▼      │                                                │
│   │  Tool Results ◀─ Tools  │                                                │
│   │              (web_search)│                                               │
│   └──────────┬─────────────┘                                                 │
│              │                                                               │
│       ┌──────▼───────┐                                                       │
│       │ Outbound Queue│                                                       │
│       └──────┬───────┘                                                       │
│              │                                                               │
│              ▼                                                               │
│       ┌──────────────┐                                                       │
│       │   Channel    │───▶ Routes to appropriate channel (telegram/ws/cli)   │
│       │   Manager    │                                                       │
│       └──────────────┘                                                       │
│                                                                              │
│   ┌──────────────────────────────────────────┐                               │
│   │  SPIFFS (12 MB)                          │                               │
│   │  /spiffs/config/  SOUL.md, USER.md       │                               │
│   │  /spiffs/memory/  MEMORY.md, YYYY-MM-DD  │                               │
│   │  /spiffs/sessions/ tg_<chat_id>.jsonl    │                               │
│   └──────────────────────────────────────────┘                               │
└─────────────────────────────────────────────────────────────────────────────┘
                             │
                             │  Anthropic Messages API (HTTPS)
                             │  + Brave Search API (HTTPS)
                             ▼
                    ┌───────────┐   ┌──────────────┐
                    │ Claude API │   │ Brave Search │
                    └───────────┘   └──────────────┘
```

---

## Architecture Refactor: Channel & Command System

### Overview

The architecture has been refactored to support a **unified Channel and Command system**, enabling code reuse across CLI, Telegram, and WebSocket channels.

### Key Changes

1. **Unified Channel Layer** (`main/channels/`)
   - All communication channels implement the same `channel_t` interface
   - Channel Manager handles lifecycle (init, start, stop, destroy)
   - Easy to add new channels (Telegram, WebSocket, future protocols)

2. **Shared Command System** (`main/commands/`)
   - Commands are registered once and shared across all channels
   - Consistent command interface: `/help`, `/session`, `/set`, `/ask`, `/memory_read`, `/exit`
   - Command context includes channel, session_id, user_id

3. **Message Flow**
   - Inbound: Channel → Message Bus → Agent Loop
   - Outbound: Agent Loop → Channel Manager → Appropriate Channel

### Directory Structure

```
main/
├── channels/                    # Unified Channel Layer
│   ├── channel.h                # Channel interface definition
│   ├── channel_manager.c/h      # Channel lifecycle management
│   ├── cli/                     # CLI Channel implementation
│   │   ├── cli_channel.c/h
│   │   └── terminal_stdio.c/h
│   ├── telegram/                # Telegram Channel (to be migrated)
│   │   └── telegram_bot.c/h
│   └── gateway/                 # WebSocket Channel (to be migrated)
│       └── ws_server.c/h
├── commands/                    # Shared Command System
│   ├── command.h                # Command interface
│   ├── command_registry.c/h     # Command registration & execution
│   ├── cmd_help.c               # /help command
│   ├── cmd_session.c            # /session command
│   ├── cmd_set.c                # /set command
│   ├── cmd_ask.c                # /ask command
│   ├── cmd_memory.c             # /memory_read command
│   └── cmd_exit.c               # /exit command
├── cli/                         # CLI Editor (transport agnostic)
│   ├── cli_terminal.c/h         # Terminal framework
│   ├── editor.c/h               # Line editor with history
│   └── cli.md                   # Documentation
└── bus/
    └── message_bus.c/h          # Message routing
```

---

## Data Flow

### Inbound Message Flow

```
1. User sends message (Telegram / WebSocket / CLI)
2. Channel receives message via its transport layer
3. Channel wraps message in mimi_msg_t with channel identifier
4. Message pushed to Inbound Queue (FreeRTOS xQueue)
5. Agent Loop (Core 1) pops message:
   a. Load session history from SPIFFS (JSONL)
   b. Build system prompt (SOUL.md + USER.md + MEMORY.md + recent notes + tool guidance)
   c. Build cJSON messages array (history + current message)
   d. ReAct loop (max 10 iterations):
      i.   Call Claude API via HTTPS (non-streaming, with tools array)
      ii.  Parse JSON response → text blocks + tool_use blocks
      iii. If stop_reason == "tool_use":
           - Execute each tool (e.g. web_search → Brave Search API)
           - Append assistant content + tool_result to messages
           - Continue loop
      iv.  If stop_reason == "end_turn": break with final text
   e. Save user message + final assistant text to session file
   f. Push response to Outbound Queue
```

### Outbound Message Flow

```
6. Channel Manager routes response by channel field:
   - "telegram" → Telegram Channel → sendMessage API
   - "websocket" → WebSocket Channel → WS frame
   - "cli" → CLI Channel → STDIO output
7. User receives reply
```

### Command Execution Flow

```
CLI/Telegram/WebSocket
        │
        ▼
┌───────────────┐
│ Channel receives│
│   input        │
└───────┬───────┘
        │
        ▼
┌───────────────┐     ┌─────────────────┐
│  Parse command │────▶│  Is it a /cmd?  │
└───────────────┘     └────────┬────────┘
                               │
                    ┌──────────┴──────────┐
                    │                     │
                   Yes                   No
                    │                     │
                    ▼                     ▼
          ┌─────────────────┐    ┌─────────────────┐
          │ Command System  │    │  Treat as chat  │
          │   Execute       │    │  message → LLM  │
          └────────┬────────┘    └─────────────────┘
                   │
                   ▼
          ┌─────────────────┐
          │  Send response  │
          │  via Channel    │
          └─────────────────┘
```

---

## Module Map

```
main/
├── mimi.c                  Entry point — app_main() orchestrates init + startup
├── mimi_config.h           All compile-time constants + build-time secrets include
├── mimi_secrets.h          Build-time credentials (gitignored, highest priority)
├── mimi_secrets.h.example  Template for mimi_secrets.h
│
├── channels/               # Unified Channel Layer (NEW)
│   ├── channel.h           # Channel interface definition
│   ├── channel_manager.c/h # Channel lifecycle management
│   ├── cli/                # CLI Channel
│   │   ├── cli_channel.c/h
│   │   └── terminal_stdio.c/h
│   ├── telegram/           # Telegram Channel (migrating)
│   │   └── telegram_bot.c/h
│   └── gateway/            # WebSocket Channel (migrating)
│       └── ws_server.c/h
│
├── commands/               # Shared Command System (NEW)
│   ├── command.h           # Command interface
│   ├── command_registry.c/h # Command registration & execution
│   ├── cmd_help.c          # /help command
│   ├── cmd_session.c       # /session command
│   ├── cmd_set.c           # /set command
│   ├── cmd_ask.c           # /ask command
│   ├── cmd_memory.c        # /memory_read command
│   └── cmd_exit.c          # /exit command
│
├── cli/                    # CLI Editor Framework
│   ├── cli_terminal.c/h    # Terminal abstraction layer
│   ├── editor.c/h          # Line editor with history & completion
│   └── cli.md              # Documentation
│
├── bus/
│   ├── message_bus.h       # mimi_msg_t struct, queue API
│   └── message_bus.c       # Two FreeRTOS queues: inbound + outbound
│
├── wifi/
│   ├── wifi_manager.h      # WiFi STA lifecycle API
│   └── wifi_manager.c      # Event handler, exponential backoff
│
├── telegram/               # (Being migrated to channels/telegram/)
│   ├── telegram_bot.h      # Bot init/start, send_message API
│   └── telegram_bot.c      # Long polling loop, JSON parsing
│
├── llm/
│   ├── llm_proxy.h         # llm_chat() + llm_chat_tools() API
│   └── llm_proxy.c         # Anthropic Messages API implementation
│
├── agent/
│   ├── agent_loop.h        # Agent task init/start
│   ├── agent_loop.c        # ReAct loop: LLM call → tool execution
│   ├── context_builder.h   # System prompt + messages builder
│   └── context_builder.c   # Reads bootstrap files + memory
│
├── tools/
│   ├── tool_registry.h     # Tool definition struct, register/dispatch
│   ├── tool_registry.c     # Tool registration, JSON schema builder
│   ├── tool_web_search.h   # Web search tool API
│   └── tool_web_search.c   # Brave Search API via HTTPS
│
├── memory/
│   ├── memory_store.h      # Long-term + daily memory API
│   ├── memory_store.c      # MEMORY.md read/write
│   ├── session_mgr.h       # Per-chat session API
│   └── session_mgr.c       # JSONL session files
│
├── gateway/                # (Being migrated to channels/gateway/)
│   ├── ws_server.h         # WebSocket server API
│   └── ws_server.c         # ESP HTTP server with WS upgrade
│
├── proxy/
│   ├── http_proxy.h        # Proxy connection API
│   └── http_proxy.c        # HTTP CONNECT tunnel + TLS
│
└── ota/
    ├── ota_manager.h       # OTA update API
    └── ota_manager.c       # esp_https_ota wrapper
```

---

## Channel Interface

All channels implement the `channel_t` interface:

```c
typedef struct channel {
    const char *name;
    const char *description;
    
    /* Lifecycle */
    mimi_err_t (*init)(struct channel *ch, const channel_config_t *cfg);
    mimi_err_t (*start)(struct channel *ch);
    mimi_err_t (*stop)(struct channel *ch);
    void (*destroy)(struct channel *ch);
    
    /* Messaging */
    mimi_err_t (*send)(struct channel *ch, const char *session_id, 
                       const char *content);
    
    /* Callbacks */
    void (*on_message)(struct channel *ch, const char *session_id,
                       const char *content, void *user_data);
    void (*on_connect)(struct channel *ch, const char *session_id,
                       void *user_data);
    void (*on_disconnect)(struct channel *ch, const char *session_id,
                          void *user_data);
    
    /* State */
    bool is_initialized;
    bool is_started;
    void *priv_data;
} channel_t;
```

### Channel Manager API

```c
/* Initialize channel manager */
mimi_err_t channel_manager_init(void);

/* Register a channel */
mimi_err_t channel_register(channel_t *ch);

/* Start/stop all channels */
mimi_err_t channel_start_all(void);
void channel_stop_all(void);

/* Send message through a channel */
mimi_err_t channel_send(const char *channel_name, const char *session_id,
                        const char *content);

/* Find a channel by name */
channel_t* channel_find(const char *name);
```

---

## Command System Interface

Commands are shared across all channels:

```c
/* Command context */
typedef struct {
    const char *channel;      // Channel name ("cli", "telegram", "websocket")
    const char *session_id;   // Session identifier
    const char *user_id;      // User identifier
    void *user_data;          // Channel-specific data
} command_context_t;

/* Command definition */
typedef struct {
    const char *name;         // Command name (without "/")
    const char *description;  // Help description
    const char *usage;        // Usage syntax
    int (*execute)(const char **args, int arg_count,
                   const command_context_t *ctx,
                   char *output, size_t output_len);
} command_t;

/* Register a command */
void command_register(const command_t *cmd);

/* Execute a command from input string */
int command_execute(const char *input, const command_context_t *ctx,
                    char *output, size_t output_len);

/* Get tab completions */
int command_get_completions(const char *prefix, char **out_matches, int max_matches);
```

### Available Commands

| Command | Description | Example |
|---------|-------------|---------|
| `/help` | Show available commands | `/help` |
| `/session` | Session management | `/session list`, `/session new abc`, `/session use abc` |
| `/set` | Set configuration | `/set key value` |
| `/ask` | Ask AI a question | `/ask What is the weather?` |
| `/memory_read` | Read memory file | `/memory_read` |
| `/exit` | Exit application | `/exit` |

---

## FreeRTOS Task Layout

| Task               | Core | Priority | Stack  | Description                          |
|--------------------|------|----------|--------|--------------------------------------|
| `tg_poll`          | 0    | 5        | 12 KB  | Telegram long polling (30s timeout)  |
| `ws_server`        | 0    | 5        | 8 KB   | WebSocket server (port 18789)        |
| `stdio_cli`        | 0    | 3        | 8 KB   | CLI terminal input handling          |
| `agent_loop`       | 1    | 6        | 12 KB  | Message processing + Claude API call |
| `outbound`         | 0    | 5        | 8 KB   | Route responses to channels          |
| httpd (internal)   | 0    | 5        | —      | WebSocket HTTP server                |
| wifi_event (IDF)   | 0    | 8        | —      | WiFi event handling (ESP-IDF)        |

**Core allocation strategy**: Core 0 handles I/O (network, serial, WiFi). Core 1 is dedicated to the agent loop (CPU-bound JSON building + waiting on HTTPS).

---

## Memory Budget

| Purpose                            | Location       | Size     |
|------------------------------------|----------------|----------|
| FreeRTOS task stacks               | Internal SRAM  | ~40 KB   |
| WiFi buffers                       | Internal SRAM  | ~30 KB   |
| TLS connections x2 (Telegram + Claude) | PSRAM      | ~120 KB  |
| JSON parse buffers                 | PSRAM          | ~32 KB   |
| Session history cache              | PSRAM          | ~32 KB   |
| System prompt buffer               | PSRAM          | ~16 KB   |
| LLM response stream buffer         | PSRAM          | ~32 KB   |
| Remaining available                | PSRAM          | ~7.7 MB  |

Large buffers (32 KB+) are allocated from PSRAM via `heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM)`.

---

## Flash Partition Layout

```
Offset      Size      Name        Purpose
─────────────────────────────────────────────
0x009000    24 KB     nvs         ESP-IDF internal use (WiFi calibration etc.)
0x00F000     8 KB     otadata     OTA boot state
0x011000     4 KB     phy_init    WiFi PHY calibration
0x020000     2 MB     ota_0       Firmware slot A
0x220000     2 MB     ota_1       Firmware slot B
0x420000    12 MB     spiffs      Markdown memory, sessions, config
0xFF0000    64 KB     coredump    Crash dump storage
```

Total: 16 MB flash.

---

## Storage Layout (SPIFFS)

SPIFFS is a flat filesystem — no real directories. Files use path-like names.

```
/spiffs/config/SOUL.md          AI personality definition
/spiffs/config/USER.md          User profile
/spiffs/memory/MEMORY.md        Long-term persistent memory
/spiffs/memory/2026-02-05.md    Daily notes (one file per day)
/spiffs/sessions/tg_12345.jsonl Session history (one file per Telegram chat)
/spiffs/sessions/cli_default.jsonl CLI session history
```

Session files are JSONL (one JSON object per line):
```json
{"role":"user","content":"Hello","ts":1738764800}
{"role":"assistant","content":"Hi there!","ts":1738764802}
```

---

## Virtual File System (VFS) Architecture

### Overview

MimiClaw uses a virtual file system (VFS) layer that abstracts underlying file system operations. This allows consistent file access across different platforms (ESP32 SPIFFS, POSIX systems).

### VFS Base Directory

The VFS base directory is configured based on the platform:

- **Linux/POSIX**: `~/.mimiclaw/workspace/`
- **Windows**: `C:\Users\<username>\.mimiclaw\workspace\`
- **ESP32**: `/spiffs/`

### Directory Structure

```
~/.mimiclaw/
├── config.json          # Configuration file (outside VFS)
└── workspace/           # VFS base directory
    ├── AGENTS.md        # Agent configuration
    ├── HEARTBEAT.md     # Heartbeat status
    ├── TOOLS.md         # Tool usage documentation
    ├── config/          # Configuration files
    │   ├── SOUL.md      # AI personality
    │   └── USER.md      # User profile
    ├── cron.json        # Cron jobs configuration
    ├── memory/          # Memory files
    │   ├── MEMORY.md    # Long-term memory
    │   └── daily/       # Daily notes
    ├── sessions/        # Session files
    │   ├── cli_default.jsonl  # CLI session
    │   ├── tg_12345.jsonl     # Telegram session
    │   └── ws_client1.jsonl    # WebSocket session
    └── skills/          # Skill files
        ├── daily-briefing.md   # Daily briefing skill
        ├── skill-creator.md    # Skill creator skill
        └── weather.md          # Weather skill
```

### File Access Pattern

All modules (session, skills, tools) use **VFS paths** (relative to the VFS base directory) for file operations:

- **Session Manager**: Uses `sessions/<session_id>.jsonl` paths for storing chat history in JSONL format
- **Skills System**: Uses `skills/<skill_name>.md` paths for skill definitions and configurations
- **Memory Store**: Uses `memory/MEMORY.md` for long-term memory and `memory/daily/<date>.md` for daily notes
- **Context Builder**: Uses `config/SOUL.md` for AI personality and `config/USER.md` for user profile
- **Tools System**: Uses VFS paths for tool configurations and data storage

### VFS Path vs Real Path Usage

| Component | Path Type | Description |
|-----------|-----------|-------------|
| Session Manager | VFS Path | Uses relative paths like `sessions/cli_default.jsonl` |
| Skills System | VFS Path | Uses relative paths like `skills/weather.md` |
| Memory Store | VFS Path | Uses relative paths like `memory/MEMORY.md` |
| Tools System | VFS Path | Uses relative paths for tool-specific data |
| Configuration | Real Path | Uses absolute path to `~/.mimiclaw/config.json` (outside VFS) |

### Detailed Directory Structure

```
~/.mimiclaw/            # Base directory (real path)
├── config.json          # Configuration file (real path, outside VFS)
└── workspace/           # VFS base directory
    ├── AGENTS.md        # Agent configuration
    ├── HEARTBEAT.md     # Heartbeat status
    ├── TOOLS.md         # Tool usage documentation
    ├── config/          # Configuration files
    │   ├── SOUL.md      # AI personality
    │   └── USER.md      # User profile
    ├── cron.json        # Cron jobs configuration
    ├── memory/          # Memory files
    │   ├── MEMORY.md    # Long-term memory
    │   └── daily/       # Daily notes
    │       ├── 2024-01-01.md
    │       └── 2024-01-02.md
    ├── sessions/        # Session files
    │   ├── cli_default.jsonl  # CLI session
    │   ├── tg_12345.jsonl     # Telegram session
    │   └── ws_client1.jsonl    # WebSocket session
    └── skills/          # Skill files
        ├── daily-briefing.md   # Daily briefing skill
        ├── skill-creator.md    # Skill creator skill
        └── weather.md          # Weather skill
```

### Session File Usage

Session files store chat history in JSONL format, with one JSON object per line:

```json
{"role":"user","content":"Hello","ts":1738764800}
{"role":"assistant","content":"Hi there!","ts":1738764802}
```

- **Location**: `sessions/<session_id>.jsonl` (VFS path)
- **Naming convention**: `{channel}_{identifier}.jsonl`
  - CLI: `cli_{session_name}.jsonl`
  - Telegram: `tg_{chat_id}.jsonl`
  - WebSocket: `ws_{client_id}.jsonl`

### Skills File Usage

Skill files are Markdown documents that define skill behavior and configuration:

- **Location**: `skills/<skill_name>.md` (VFS path)
- **Format**: Markdown with YAML frontmatter for metadata
- **Purpose**: Define skill functionality, triggers, and responses

### Tools File Usage

Tools may use VFS paths for configuration and data storage:

- **Configuration**: Stored in `tools/<tool_name>.json` (VFS path)
- **Data**: Stored in `tools/<tool_name>/` directories (VFS paths)
- **Usage**: Tools access their data using relative VFS paths

### VFS API

```c
/* File operations */
mimi_err_t mimi_fs_open(const char *path, const char *mode, mimi_file_t **out_file);
mimi_err_t mimi_fs_close(mimi_file_t *file);
mimi_err_t mimi_fs_read(mimi_file_t *file, void *buf, size_t len, size_t *out_read);
mimi_err_t mimi_fs_write(mimi_file_t *file, const void *buf, size_t len, size_t *out_written);
mimi_err_t mimi_fs_seek(mimi_file_t *file, int offset, int whence);

/* Directory operations */
mimi_err_t mimi_fs_mkdir(const char *path);
mimi_err_t mimi_fs_mkdir_p(const char *path);

/* Path operations */
bool mimi_fs_exists(const char *path);
```

### Direct File System Access

For files outside the VFS (like the main `config.json`), direct POSIX file operations are used:

```c
/* Direct POSIX API functions (bypassing VFS) */
bool mimi_fs_exists_direct(const char *path);
int mimi_fs_mkdir_p_direct(const char *dir);
```

---

## Configuration

MimiClaw uses a dual-configuration approach:

### 1. Build-time Configuration (`mimi_secrets.h`)

For sensitive credentials and platform-specific settings:

| Define                       | Description                             |
|------------------------------|-----------------------------------------|
| `MIMI_SECRET_WIFI_SSID`     | WiFi SSID                               |
| `MIMI_SECRET_WIFI_PASS`     | WiFi password                           |
| `MIMI_SECRET_TG_TOKEN`      | Telegram Bot API token                  |
| `MIMI_SECRET_API_KEY`       | Anthropic API key                       |
| `MIMI_SECRET_MODEL`         | Model ID (default: claude-opus-4-6)     |
| `MIMI_SECRET_PROXY_HOST`    | HTTP proxy hostname/IP (optional)       |
| `MIMI_SECRET_PROXY_PORT`    | HTTP proxy port (optional)              |
| `MIMI_SECRET_SEARCH_KEY`    | Brave Search API key (optional)         |

### 2. Runtime Configuration (`config.json`)

For runtime settings stored in the user's home directory:

**Location:**
- **Linux/POSIX**: `~/.mimiclaw/config.json`
- **Windows**: `C:\Users\<username>\.mimiclaw\config.json`

**Example config.json:**
```json
{
  "agents": {
    "defaults": {
      "workspace": "~/.mimiclaw/workspace",
      "timezone": "PST8PDT,M3.2.0,M11.1.0",
      "model": "nvidia/nemotron-nano-9b-v2:free",
      "provider": "openrouter",
      "maxTokens": 8192,
      "temperature": 0.1,
      "maxToolIterations": 40,
      "memoryWindow": 100,
      "sendWorkingStatus": true
    }
  },
  "channels": {
    "telegram": {
      "enabled": true,
      "token": "YOUR_TELEGRAM_TOKEN",
      "allowFrom": ["123456789"]
    }
  },
  "providers": {
    "openrouter": {
      "apiKey": "YOUR_API_KEY",
      "apiBase": "https://openrouter.ai/api/v1/chat/completions"
    }
  },
  "proxy": {
    "host": "",
    "port": "",
    "type": "http"
  },
  "tools": {
    "search": {
      "apiKey": "YOUR_SEARCH_KEY"
    }
  },
  "logging": {
    "level": "info"
  }
}
```

### Configuration Priority

1. `mimi_secrets.h` (build-time) - highest priority
2. `config.json` (runtime) - overrides defaults
3. Built-in defaults - lowest priority

NVS is still initialized (required by ESP-IDF WiFi internals) but is not used for application configuration.

---

## Message Bus Protocol

The internal message bus uses two FreeRTOS queues carrying `mimi_msg_t`:

```c
typedef struct {
    char channel[16];   // "telegram", "websocket", "cli"
    char chat_id[32];   // Telegram chat ID or WS client ID or CLI session
    char *content;      // Heap-allocated text (ownership transferred)
} mimi_msg_t;
```

- **Inbound queue**: channels → agent loop (depth: 8)
- **Outbound queue**: agent loop → dispatch → channels (depth: 8)
- Content string ownership is transferred on push; receiver must `free()`.

---

## WebSocket Protocol

Port: **18789**. Max clients: **4**.

**Client → Server:**
```json
{"type": "message", "content": "Hello", "chat_id": "ws_client1"}
```

**Server → Client:**
```json
{"type": "response", "content": "Hi there!", "chat_id": "ws_client1"}
```

Client `chat_id` is auto-assigned on connection (`ws_<fd>`) but can be overridden in the first message.

---

## Claude API Integration

Endpoint: `POST https://api.anthropic.com/v1/messages`

Request format (Anthropic-native, non-streaming, with tools):
```json
{
  "model": "claude-opus-4-6",
  "max_tokens": 4096,
  "system": "<system prompt>",
  "tools": [
    {
      "name": "web_search",
      "description": "Search the web for current information.",
      "input_schema": {"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]}
    }
  ],
  "messages": [
    {"role": "user", "content": "Hello"},
    {"role": "assistant", "content": "Hi!"},
    {"role": "user", "content": "What's the weather today?"}
  ]
}
```

Key difference from OpenAI: `system` is a top-level field, not inside the `messages` array.

Non-streaming JSON response:
```json
{
  "id": "msg_xxx",
  "type": "message",
  "role": "assistant",
  "content": [
    {"type": "text", "text": "Let me search for that."},
    {"type": "tool_use", "id": "toolu_xxx", "name": "web_search", "input": {"query": "weather today"}}
  ],
  "stop_reason": "tool_use"
}
```

When `stop_reason` is `"tool_use"`, the agent loop executes each tool and sends results back:
```json
{"role": "assistant", "content": [<text + tool_use blocks>]}
{"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_xxx", "content": "..."}]}
```

The loop repeats until `stop_reason` is `"end_turn"` (max 10 iterations).

---

## Startup Sequence

```
app_main()
  ├── init_nvs()                    NVS flash init (erase if corrupted)
  ├── esp_event_loop_create_default()
  ├── mimi_fs_init()                Initialize VFS layer with base directory
  ├── mimi_config_load()            Load config.json from ~/.mimiclaw/ (real path)
  ├── message_bus_init()            Create inbound + outbound queues
  ├── memory_store_init()           Initialize memory store with VFS paths
  ├── session_mgr_init()            Initialize session manager with VFS paths
  ├── skills_system_init()          Initialize skills system with VFS paths
  ├── tools_system_init()           Initialize tools system with VFS paths
  ├── 
  ├── Initialize Channel System (NEW)
  │   ├── command_system_init()     Initialize command registry
  │   ├── cmd_help_init()           Register /help command
  │   ├── cmd_session_init()        Register /session command
  │   ├── cmd_set_init()            Register /set command
  │   ├── cmd_memory_read_init()    Register /memory_read command
  │   ├── cmd_ask_init()            Register /ask command
  │   ├── cmd_exit_init()           Register /exit command
  │   ├── channel_manager_init()    Initialize channel manager
  │   ├── cli_channel_init()        Initialize CLI channel
  │   └── channel_register()        Register CLI channel
  │
  ├── wifi_manager_init()           Init WiFi STA mode
  ├── http_proxy_init()             Load proxy config
  ├── telegram_bot_init()           Load bot token
  ├── llm_proxy_init()              Load API key + model
  ├── tool_registry_init()          Register tools
  ├── agent_loop_init()
  │
  ├── wifi_manager_start()          Connect to WiFi
  │   └── wifi_manager_wait_connected(30s)
  │
  └── [if WiFi connected]
      ├── channel_start_all()       Start all registered channels (NEW)
      ├── telegram_bot_start()      Start Telegram polling
      ├── agent_loop_start()        Start agent processing
      ├── ws_server_start()         Start WebSocket server
      └── outbound_dispatch task    Start outbound routing
```

---

## Nanobot Reference Mapping

| Nanobot Module              | MimiClaw Equivalent            | Notes                        |
|-----------------------------|--------------------------------|------------------------------|
| `agent/loop.py`             | `agent/agent_loop.c`           | ReAct loop with tool use     |
| `agent/context.py`          | `agent/context_builder.c`      | Loads SOUL.md + USER.md + memory |
| `agent/memory.py`           | `memory/memory_store.c`        | MEMORY.md + daily notes      |
| `session/manager.py`        | `memory/session_mgr.c`         | JSONL per chat, ring buffer  |
| `channels/telegram.py`      | `channels/telegram/`           | Migrated to Channel system   |
| `channels/gateway.py`       | `channels/gateway/`            | Migrated to Channel system   |
| `channels/cli.py`           | `channels/cli/`                | New Channel implementation   |
| `commands/registry.py`      | `commands/command_registry.c`  | Shared command system        |
| `bus/events.py` + `queue.py`| `bus/message_bus.c`            | FreeRTOS queues              |
| `providers/litellm_provider.py` | `llm/llm_proxy.c`         | Direct Anthropic API         |
| `config/schema.py`          | `mimi_config.h` + `mimi_secrets.h` | Build-time secrets    |
| `cli/commands.py`           | `commands/`                    | Shared across all channels   |
| `agent/tools/*`             | `tools/tool_registry.c` + `tool_web_search.c` | web_search via Brave |

---

## Migration Status

| Component | Status | Location |
|-----------|--------|----------|
| CLI Channel | ✅ Completed | `channels/cli/` |
| Command System | ✅ Completed | `commands/` |
| Telegram Channel | 🔄 In Progress | `channels/telegram/` |
| WebSocket Channel | 🔄 In Progress | `channels/gateway/` |
| Message Bus | ✅ Compatible | `bus/message_bus.c` |

