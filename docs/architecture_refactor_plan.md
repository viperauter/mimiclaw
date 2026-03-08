# Mimiclaw Architecture Refactor Plan

## Overview

Refactor the CLI architecture to support a unified Channel and Command system, enabling code reuse across CLI, Telegram, and WebSocket channels.

## Current Issues

1. **Scattered Implementation**: CLI, Telegram, and WebSocket are implemented separately
2. **No Command Sharing**: Each channel implements its own command handling
3. **Poor Extensibility**: Adding new channels requires reimplementing command logic
4. **No Gateway Abstraction**: Channels mix business logic with protocol implementation
5. **Protocol Duplication**: Each Channel implements its own WS/HTTP transport

## Target Architecture

```
main/
├── channels/               # Channel Layer (Business Logic)
│   ├── channel.h           # Channel Interface
│   ├── channel_manager.c/h # Channel Manager
│   ├── cli/                # CLI Channel
│   │   ├── cli_channel.c/h
│   │   └── cli_gateway_adapter.c
│   ├── telegram/           # Telegram Channel
│   │   └── telegram_channel.c/h
│   ├── feishu/             # Feishu Channel
│   │   └── feishu_channel.c/h
│   └── qq/                 # QQ Channel
│       └── qq_channel.c/h
│
├── gateway/                # Gateway Layer (Protocol Abstraction) ★ NEW
│   ├── gateway.h           # Gateway Interface
│   ├── gateway_manager.c/h # Gateway Lifecycle Management
│   ├── stdio/              # STDIO Gateway
│   │   └── stdio_gateway.c/h
│   ├── websocket/          # WebSocket Gateway
│   │   ├── ws_server.c/h   # WS Server
│   │   └── ws_client.c/h   # WS Client ★ NEW
│   └── http/               # HTTP Gateway
│       └── http_gateway.c/h
│
├── router/                  # Unified Input Routing ★ NEW
│   ├── router.h
│   └── router.c            # Unified command/message routing
│
├── cli/                    # CLI Editor (Interaction Layer)
│   ├── cli_terminal.c/h    # Terminal framework
│   ├── editor.c/h          # Line editor core
│   └── cli.md
│
├── commands/               # Shared Command System
│   ├── command.h           # Command Interface
│   ├── command_registry.c/h
│   ├── cmd_session.c       # /session command
│   ├── cmd_help.c          # /help command
│   └── ...
│
├── telegram/               # (Being migrated to channels/telegram/)
├── gateway/                # (Being migrated to gateway/)
└── bus/
    └── message_bus.c/h
```

## Architecture Layers

```
┌─────────────────────────────────────────┐
│  Channel Layer (Business Logic)         │
│  ├─ CLI Channel                         │
│  ├─ Telegram Channel                    │
│  ├─ Feishu Channel                      │
│  └─ QQ Channel                          │
│       │                                 │
│       └─ All use Gateway interface      │
└───────┬─────────────────────────────────┘
        │
┌───────▼─────────────────────────────────┐
│  Router (Unified Routing)               │ ★ NEW
│  1. Receive input from any Gateway      │
│  2. Check if starts with "/"            │
│  3. Yes → Command System                │
│  4. No  → Message Bus (Agent)           │
└───────┬─────────────────────────────────┘
        │
┌───────▼─────────────────────────────────┐
│  Gateway Layer (Protocol Abstraction)   │ ★ NEW
│  ├─ STDIO Gateway                       │
│  ├─ WebSocket Server Gateway            │
│  ├─ WebSocket Client Gateway            │
│  └─ HTTP Client Gateway                 │
│       │                                 │
│       └─ Reusable across Channels       │
└─────────────────────────────────────────┘
```

## Execution Steps

### Phase 1: Create Gateway Framework

**Goal**: Establish Gateway abstraction layer for protocol reuse

**Commit 1.1: Create Gateway interface**

Files to create:
- `main/gateway/gateway.h` - Gateway interface definition
- `main/gateway/gateway_manager.h` - Gateway manager header
- `main/gateway/gateway_manager.c` - Gateway manager implementation

Key interfaces:
```c
typedef struct gateway {
    const char *name;
    gateway_type_t type;
    mimi_err_t (*init)(gateway_t *gw, const gateway_config_t *cfg);
    mimi_err_t (*start)(gateway_t *gw);
    mimi_err_t (*send)(gateway_t *gw, const char *session_id, const char *content);
    void (*set_on_message)(gateway_t *gw, gateway_on_message_cb_t cb, void *user_data);
    // ...
} gateway_t;
```

**Commit 1.2: Create Gateway Manager**

Files to modify:
- `main/mimi.c` - Add gateway_manager_init() call

**Verification**:
- [ ] Gateway manager initializes successfully
- [ ] Can register and find gateways

### Phase 2: Create Router

**Goal**: Unified command/message routing for all channels

**Commit 2.1: Create Router interface**

Files to create:
- `main/router/router.h` - Router header
- `main/router/router.c` - Router implementation

Key functions:
```c
mimi_err_t router_init(void);
mimi_err_t router_handle(gateway_t *gw, const char *session_id, 
                          const char *content);
mimi_err_t router_register_mapping(const char *gateway_name, 
                                    const char *channel_name);
```

**Commit 2.2: Integrate Router with Command System**

Files to modify:
- `main/router/router.c` - Connect to command_execute()
- `main/router/router.c` - Connect to message_bus_push_inbound()

**Verification**:
- [ ] Commands are routed to Command System
- [ ] Non-commands are routed to Message Bus

### Phase 3: Migrate STDIO Gateway

**Goal**: Move STDIO transport to Gateway layer

**Commit 3.1: Create STDIO Gateway**

Files to create:
- `main/gateway/stdio/stdio_gateway.h` - STDIO Gateway header
- `main/gateway/stdio/stdio_gateway.c` - STDIO Gateway implementation

Migration from:
- `main/channels/cli/terminal_stdio.c` → `main/gateway/stdio/stdio_gateway.c`

**Commit 3.2: Update CLI Channel to use STDIO Gateway**

Files to modify:
- `main/channels/cli/cli_channel.c` - Use gateway_find("stdio")
- `main/channels/cli/cli_channel.c` - Register router mapping

**Commit 3.3: Remove old terminal_stdio files**

Files to remove:
- `main/channels/cli/terminal_stdio.c`
- `main/channels/cli/terminal_stdio.h`

**Verification**:
- [ ] CLI still works with STDIO
- [ ] Commands work in CLI
- [ ] Non-commands go to Agent

### Phase 4: Migrate WebSocket Gateway

**Goal**: Separate WS server from WS Channel

**Commit 4.1: Create WebSocket Server Gateway**

Files to create:
- `main/gateway/websocket/ws_server.h` - WS Server Gateway header
- `main/gateway/websocket/ws_server.c` - WS Server Gateway implementation

Migration from:
- `main/channels/websocket/ws_channel.c` → Extract WS server logic

**Commit 4.2: Create WebSocket Client Gateway**

Files to create:
- `main/gateway/websocket/ws_client.h` - WS Client Gateway header
- `main/gateway/websocket/ws_client.c` - WS Client Gateway implementation

**Commit 4.3: Update WebSocket Channel**

Files to modify:
- `main/channels/websocket/ws_channel.c` - Use ws_server Gateway
- `main/channels/websocket/ws_channel.c` - Register router mapping

**Verification**:
- [ ] WebSocket server starts correctly
- [ ] WS clients can connect
- [ ] Messages route through Input Processor

### Phase 5: Create HTTP Gateway

**Goal**: Unified HTTP client for Channels

**Commit 5.1: Create HTTP Gateway**

Files to create:
- `main/gateway/http/http_gateway.h` - HTTP Gateway header
- `main/gateway/http/http_gateway.c` - HTTP Gateway implementation

Features:
- GET/POST/PUT/DELETE methods
- JSON request/response handling
- Authentication header support

**Commit 5.2: Update Telegram Channel**

Files to modify:
- `main/channels/telegram/telegram_channel.c` - Use http_gateway
- Remove direct HTTP implementation from telegram_channel.c

**Verification**:
- [ ] Telegram polling works
- [ ] Telegram send_message works

### Phase 6: Create Feishu Channel

**Goal**: Demonstrate Gateway reuse with new Channel

**Commit 6.1: Create Feishu Channel**

Files to create:
- `main/channels/feishu/feishu_channel.h` - Feishu Channel header
- `main/channels/feishu/feishu_channel.c` - Feishu Channel implementation

Uses:
- `gateway/websocket/ws_client.c` for WebSocket connection
- `router/router.c` for command routing

**Commit 6.2: Register Feishu Channel**

Files to modify:
- `main/mimi.c` - Register Feishu Channel
- `main/config.h` - Add Feishu config options

**Verification**:
- [ ] Feishu WebSocket connects
- [ ] Feishu messages route correctly
- [ ] Commands work in Feishu

### Phase 7: Create QQ Channel

**Goal**: Further demonstrate Gateway reuse

**Commit 7.1: Create QQ Channel**

Files to create:
- `main/channels/qq/qq_channel.h` - QQ Channel header
- `main/channels/qq/qq_channel.c` - QQ Channel implementation

Uses:
- `gateway/websocket/ws_client.c` (same as Feishu)
- `router/router.c` (same as all channels)

**Verification**:
- [ ] QQ WebSocket connects
- [ ] QQ messages route correctly

### Phase 8: Final Integration and Cleanup

**Commit 8.1: Update Architecture Documentation**

Files to modify:
- `docs/ARCHITECTURE.md` - Update with new Gateway layer
- `docs/architecture_refactor_plan.md` - Mark all steps complete

**Commit 8.2: Final Testing**

Test matrix:
- [ ] CLI commands work
- [ ] CLI chat messages work
- [ ] Telegram commands work
- [ ] Telegram chat messages work
- [ ] WebSocket commands work
- [ ] WebSocket chat messages work
- [ ] Feishu commands work (if configured)
- [ ] Feishu chat messages work (if configured)

## Detailed Implementation

### Step 1.1: Gateway Interface

```c
/* gateway/gateway.h */

typedef enum {
    GATEWAY_TYPE_STDIO,
    GATEWAY_TYPE_WS_SERVER,
    GATEWAY_TYPE_WS_CLIENT,
    GATEWAY_TYPE_HTTP_CLIENT
} gateway_type_t;

typedef struct gateway gateway_t;

typedef void (*gateway_on_message_cb_t)(gateway_t *gw, const char *session_id, 
                                        const char *content, void *user_data);

struct gateway {
    const char *name;
    gateway_type_t type;
    
    mimi_err_t (*init)(gateway_t *gw, const gateway_config_t *cfg);
    mimi_err_t (*start)(gateway_t *gw);
    mimi_err_t (*stop)(gateway_t *gw);
    void (*destroy)(gateway_t *gw);
    
    mimi_err_t (*send)(gateway_t *gw, const char *session_id, const char *content);
    
    void (*set_on_message)(gateway_t *gw, gateway_on_message_cb_t cb, void *user_data);
    void (*set_on_connect)(gateway_t *gw, gateway_on_connect_cb_t cb, void *user_data);
    void (*set_on_disconnect)(gateway_t *gw, gateway_on_disconnect_cb_t cb, void *user_data);
    
    bool is_initialized;
    bool is_started;
    void *priv_data;
};
```

### Step 2.1: Router

```c
/* router/router.c */

mimi_err_t router_handle(gateway_t *gw, const char *session_id, 
                          const char *content) {
    /* Find channel mapping */
    const char *channel_name = find_channel_for_gateway(gw->name);
    
    /* Check if command */
    if (content[0] == '/') {
        /* Execute command */
        command_context_t ctx = {
            .channel = channel_name,
            .session_id = session_id,
        };
        char output[2048];
        command_execute(content, &ctx, output, sizeof(output));
        gw->send(gw, session_id, output);
    } else {
        /* Send to Agent */
        mimi_msg_t msg = {
            .channel = channel_name,
            .chat_id = session_id,
            .content = strdup(content),
        };
        message_bus_push_inbound(&msg);
    }
}
```

### Step 6.1: Feishu Channel Example

```c
/* channels/feishu/feishu_channel.c */

mimi_err_t feishu_channel_init(channel_t *ch, const channel_config_t *cfg) {
    /* Use existing WS Client Gateway */
    gateway_t *gw = gateway_find("feishu_ws");
    if (!gw) {
        /* Create if not exists */
        gateway_config_t gw_cfg = {
            .type = GATEWAY_TYPE_WS_CLIENT,
            .name = "feishu_ws",
            .config.ws_client.url = cfg->ws_url,
        };
        gateway_create(&gw_cfg, &gw);
        gateway_register(gw);
    }
    
    /* Register mapping for Router */
    router_register_mapping("feishu_ws", "feishu");
    
    /* Set callback */
    gw->set_on_message(gw, feishu_on_message, ch);
    
    ch->priv_data = gw;
    return MIMI_OK;
}

static void feishu_on_message(gateway_t *gw, const char *session_id, 
                               const char *content, void *user_data) {
    /* Route through Router */
    router_handle(gw, session_id, content);
}
```

## Benefits

1. **Code Reuse**: Feishu and QQ share the same WS Client Gateway
2. **Clear Separation**: Gateway = Protocol, Channel = Business Logic
3. **Unified Routing**: All channels use Input Processor for command/message routing
4. **Easy Extension**: New channel only needs to configure Gateway, no protocol implementation
5. **Testability**: Gateway can be tested independently
6. **Platform Abstraction**: Gateways use platform layer abstraction for network operations
7. **Cross-platform Support**: Platform abstraction enables different implementations for different platforms

## Progress Tracking

| Phase | Step | Description | Status | Commit |
|-------|------|-------------|--------|--------|
| 1 | 1.1 | Create Gateway interface | ✅ Completed | - |
| 1 | 1.2 | Create Gateway Manager | ✅ Completed | - |
| 2 | 2.1 | Create Router interface | ✅ Completed | - |
| 2 | 2.2 | Integrate with Command System | ✅ Completed | - |
| 3 | 3.1 | Create STDIO Gateway | ✅ Completed | - |
| 3 | 3.2 | Update CLI Channel | ✅ Completed | - |
| 3 | 3.3 | Remove old terminal_stdio | ✅ Completed | - |
| 4 | 4.1 | Create WS Server Gateway | ✅ Completed | - |
| 4 | 4.2 | Create WS Client Gateway | ✅ Completed | - |
| 4 | 4.3 | Update WebSocket Channel | ✅ Completed | - |
| 5 | 5.1 | Create HTTP Gateway | ✅ Completed | - |
| 5 | 5.2 | Update Telegram Channel | ✅ Completed | - |
| 6 | 6.1 | Create Feishu Channel | ✅ Completed | - |
| 6 | 6.2 | Register Feishu Channel | ✅ Completed | - |
| 7 | 7.1 | Create QQ Channel | ✅ Completed | - |
| 8 | 8.1 | Update Documentation | ✅ Completed | - |
| 8 | 8.2 | Final Testing | ✅ Completed | - |

## Current Status (Implementation Progress)

✅ Completed:
- **Phase 1: Gateway Framework**
  - ✅ 1.1: Create Gateway interface (gateway.h)
  - ✅ 1.2: Create Gateway Manager (gateway_manager.c/h)

- **Phase 2: Router**
  - ✅ 2.1: Create Router interface (router.c/h)
  - ✅ 2.2: Integrate with Command System

- **Phase 3: STDIO Gateway**
  - ✅ 3.1: Create STDIO Gateway (stdio_gateway.c/h)
  - ✅ 3.2: Update CLI Channel to use STDIO Gateway
  - ✅ 3.3: Remove old terminal_stdio files

- **Phase 4: WebSocket Gateway**
  - ✅ 4.1: Create WebSocket Server Gateway (ws_gateway.c/h)
  - ✅ 4.2: Create WebSocket Client Gateway (ws_client_gateway.c/h)
  - ✅ 4.3: Update WebSocket Channel

- **Phase 5: HTTP Gateway**
  - ✅ 5.1: Create HTTP Gateway (http_gateway.c/h)
  - ✅ 5.2: Update Telegram Channel to use HTTP Gateway

- **Phase 6: Feishu Channel**
  - ✅ 6.1: Create Feishu Channel (feishu_channel.c/h)
  - ✅ 6.2: Register Feishu Channel

- **Phase 7: QQ Channel**
  - ✅ 7.1: Create QQ Channel (qq_channel.c/h)

- **Phase 8: Platform Abstraction** (Additional)
  - ✅ 8.1: Create HTTP platform abstraction (platform/http/http.h)
  - ✅ 8.2: Create WebSocket platform abstraction (platform/websocket/websocket.h)
  - ✅ 8.3: Refactor gateways to use platform abstraction

## Build Commands

```bash
# Build
cd build && cmake .. && make -j4

# Run
./mimiclaw

# Test CLI
cd build && ./mimiclaw
# Type: /help
# Type: hello (should go to Agent)
```

## Additional Features

### Feature 1: Customization and Feature Toggling

**Goal**: Allow users to customize and toggle features (e.g., Feishu, QQ channels) during build time.

**Implementation Approach**: CMake + Configuration Header File

#### Step 1: Create Configuration Template

Create `mimiclaw_config.h.in`:

```c
/* mimiclaw_config.h.in - Configuration template */

/* Feature toggles */
#cmakedefine ENABLE_FEISHU
#cmakedefine ENABLE_QQ
#cmakedefine ENABLE_WEBSOCKET
#cmakedefine ENABLE_STDIO

/* Configuration parameters */
#define MIMICLAW_MAX_CHANNELS @MIMICLAW_MAX_CHANNELS@
#define MIMICLAW_MAX_GATEWAYS @MIMICLAW_MAX_GATEWAYS@
#define MIMICLAW_HTTP_PORT @MIMICLAW_HTTP_PORT@
```

#### Step 2: Update CMakeLists.txt

```cmake
# Feature options
option(ENABLE_FEISHU "Enable Feishu channel support" ON)
option(ENABLE_QQ "Enable QQ channel support" ON)
option(ENABLE_WEBSOCKET "Enable WebSocket support" ON)
option(ENABLE_STDIO "Enable STDIO support" ON)

# Configuration parameters
set(MIMICLAW_MAX_CHANNELS 8 CACHE STRING "Maximum number of channels")
set(MIMICLAW_MAX_GATEWAYS 8 CACHE STRING "Maximum number of gateways")
set(MIMICLAW_HTTP_PORT 18789 CACHE STRING "HTTP server port")

# Generate config header
configure_file(
    ${CMAKE_SOURCE_DIR}/include/mimiclaw_config.h.in
    ${CMAKE_BINARY_DIR}/include/mimiclaw_config.h
    @ONLY
)

include_directories(${CMAKE_BINARY_DIR}/include)
```

#### Step 3: Use Configuration in Code

```c
#include "mimiclaw_config.h"

#ifdef ENABLE_FEISHU
    if (feishu_channel_module_init() != MIMI_OK) {
        MIMI_LOGW(TAG, "feishu_channel_module_init failed");
    } else {
        if (channel_register(&g_feishu_channel) != MIMI_OK) {
            MIMI_LOGW(TAG, "Failed to register Feishu channel");
        }
    }
#endif

#ifdef ENABLE_QQ
    if (qq_channel_module_init() != MIMI_OK) {
        MIMI_LOGW(TAG, "qq_channel_module_init failed");
    } else {
        if (channel_register(&g_qq_channel) != MIMI_OK) {
            MIMI_LOGW(TAG, "Failed to register QQ channel");
        }
    }
#endif
```

#### Usage

```bash
# Build with specific features
cmake -DENABLE_FEISHU=OFF -DENABLE_QQ=OFF ..

# Full build (default)
cmake ..
```

### Feature 2: Feishu Gateway Debugging

**Goal**: Provide guidance for debugging Feishu WebSocket gateway communication.

#### Step 1: Obtain Feishu Access Token

1. **Register Developer Account**:
   - Go to [Feishu Open Platform](https://open.feishu.cn/)
   - Register and login with Feishu account

2. **Create Enterprise Self-built App**:
   - Go to Developer Console → Create App
   - Fill in app name and description
   - Select "Self-built" type

3. **Get App Credentials**:
   - In App Details → Credentials & Basic Info
   - Note down `App ID` and `App Secret`

4. **Generate Access Token**:
   - API: `POST https://open.feishu.cn/open-apis/auth/v3/app_access_token/internal/`
   - Request body:
     ```json
     {
       "app_id": "YOUR_APP_ID",
       "app_secret": "YOUR_APP_SECRET"
     }
     ```
   - Response contains `app_access_token` (valid for 2 hours)

#### Step 2: Configure Feishu in Mimiclaw

**config.json**:

```json
{
  "channels": {
    "feishu": {
      "app_id": "YOUR_APP_ID",
      "app_secret": "YOUR_APP_SECRET",
      "access_token": "YOUR_ACCESS_TOKEN"
    }
  }
}
```

#### Step 3: Debugging Tips

1. **Enable Debug Logs**:
   ```bash
   ./mimiclaw --logs debug
   ```

2. **Check Network Connectivity**:
   - Ensure server can access `open.feishu.cn`
   - Verify WebSocket connection URL

3. **Permission Setup**:
   - In Feishu Open Platform → Permission Management
   - Add required permissions (e.g., message sending)
   - Publish app or add test users

4. **API Testing**:
   - Use Feishu Open Platform's API Debug Tool
   - Test token validity and permissions

5. **Common Issues**:
   - Token expiration: Implement auto-refresh
   - Permission errors: Check app permissions
   - Connection failures: Verify network and firewall settings

#### Verification

- [ ] Feishu WebSocket connects successfully
- [ ] Messages are received and routed
- [ ] Commands work in Feishu chat
- [ ] Token refresh works automatically
