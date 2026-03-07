# Mimiclaw Architecture Refactor Plan

## Overview

Refactor the CLI architecture to support a unified Channel and Command system, enabling code reuse across CLI, Telegram, and WebSocket channels.

## Current Issues

1. **Scattered Implementation**: CLI, Telegram, and WebSocket are implemented separately
2. **No Command Sharing**: Each channel implements its own command handling
3. **Poor Extensibility**: Adding new channels requires reimplementing command logic

## Target Architecture

```
main/
├── channels/               # Unified Channel Layer
│   ├── channel.h           # Channel Interface
│   ├── channel_manager.c/h # Channel Manager
│   └── cli/                # CLI Channel
│       ├── cli_channel.c/h
│       └── terminal_stdio.c
├── commands/               # Shared Command System
│   ├── command.h           # Command Interface
│   ├── command_registry.c/h
│   ├── cmd_session.c       # /session command
│   ├── cmd_help.c          # /help command
│   └── ...
├── telegram/               # To be migrated to channels/
├── gateway/                # To be migrated to channels/
└── bus/
    └── message_bus.c/h
```

## Execution Steps

### Phase 1: Create Command System Framework

**Commit 1: Add command interface and registry**

Files to create:
- `main/commands/command.h` - Command interface definition
- `main/commands/command_registry.h` - Command registry header
- `main/commands/command_registry.c` - Command registry implementation

**Commit 2: Migrate /help command**

Files to create:
- `main/commands/cmd_help.c` - /help command implementation

**Commit 3: Migrate /session command**

Files to create:
- `main/commands/cmd_session.c` - /session command implementation

**Commit 4: Migrate remaining commands**

Files to create:
- `main/commands/cmd_exit.c` - /exit command
- `main/commands/cmd_set.c` - /set command
- `main/commands/cmd_ask.c` - /ask command
- `main/commands/cmd_memory.c` - /memory_read command

### Phase 2: Refactor CLI to CLI Channel

**Commit 5: Create CLI Channel implementation**

Files to create:
- `main/channels/cli/cli_channel.h` - CLI Channel header
- `main/channels/cli/cli_channel.c` - CLI Channel implementation

**Commit 6: Adapt terminal_stdio to new interface**

Files to modify:
- `main/channels/cli/terminal_stdio.c` - Adapt to Channel interface
- `main/channels/cli/terminal_stdio.h` - Update header

**Commit 7: Remove old CLI implementation**

Files to remove:
- `main/cli/cli_terminal.c` - Remove (functionality moved)
- `main/cli/cli_terminal.h` - Remove
- `main/cli/command.c` - Remove (migrated to commands/)
- `main/cli/command.h` - Remove

**Commit 8: Update build system**

Files to modify:
- `CMakeLists.txt` - Update source files and include paths

### Phase 3: Integration and Testing

**Commit 9: Integrate Channel and Command systems**

Files to modify:
- `main/mimi.c` - Initialize Channel Manager and register CLI Channel

**Commit 10: Final testing and bug fixes**

- Verify all commands work correctly
- Ensure backward compatibility
- Performance testing

## Detailed Implementation

### Step 1.1: Command Interface (Commit 1)

```c
/* commands/command.h */

typedef struct {
    const char *channel;
    const char *session_id;
    const char *user_id;
    void *user_data;
} command_context_t;

typedef struct {
    const char *name;
    const char *description;
    const char *usage;
    int (*execute)(const char **args, int arg_count, 
                   const command_context_t *ctx,
                   char *output, size_t output_len);
} command_t;

void command_register(const command_t *cmd);
int command_execute(const char *input, 
                    const command_context_t *ctx,
                    char *output, size_t output_len);
void command_get_help(char *output, size_t output_len);
```

### Step 2.1: CLI Channel (Commit 5)

```c
/* channels/cli/cli_channel.c */

#include "channels/channel.h"
#include "commands/command.h"

static void cli_on_line_received(const char *line, void *user_data)
{
    channel_t *ch = (channel_t *)user_data;
    
    command_context_t ctx = {
        .channel = ch->name,
        .session_id = "cli_default",
        .user_id = "local_user",
    };
    
    char output[1024];
    int ret = command_execute(line, &ctx, output, sizeof(output));
    
    if (ret == 0) {
        channel_send(ch->name, ctx.session_id, output);
    } else {
        channel_send(ch->name, ctx.session_id, "Unknown command");
    }
}

channel_t g_cli_channel = {
    .name = "cli",
    .init = cli_channel_init,
    .start = cli_channel_start,
    .stop = cli_channel_stop,
    .send = cli_channel_send,
};
```

## Progress Tracking

| Step | Description | Status | Commit |
|------|-------------|--------|--------|
| 1.1 | Command interface and registry | Completed | 374a7cc |
| 1.2 | Migrate /help command | Completed | 337b8ae |
| 1.3 | Migrate /session command | Completed | 1a39280 |
| 1.4 | Migrate remaining commands | Completed | beefc17 |
| 2.1 | Create CLI Channel | Completed | be04bf3 |
| 2.2 | Adapt terminal_stdio | Completed | edf4faf |
| 2.3 | Remove old CLI | Completed | 3cc1b36 |
| 2.4 | Update build system | Completed | 7ac49e5 |
| 3.1 | Integration | Completed | 0e68b76 |
| 3.2 | Testing | Completed | 92e3598 |

## Summary

All refactoring steps completed successfully. The architecture now has:

1. **Unified Command System** (`main/commands/`)
   - Shared command interface for all channels
   - Commands: /help, /session, /set, /memory_read, /ask, /exit
   - Command registry with lookup and execution

2. **Channel Framework** (`main/channels/`)
   - Channel interface for protocol abstraction
   - Channel manager for lifecycle management
   - CLI Channel implementation

3. **Clean Architecture**
   - Commands are shared across all channels
   - Easy to add new channels (Telegram, WebSocket)
   - Easy to add new commands

## Build Status

✅ Compilation successful

```bash
cd build && cmake .. && make -j4
```
