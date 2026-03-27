# Subagents (in-proc) — New Orchestration Interface

This document describes the **current in-process subagent orchestration** system.

In this design, a "subagent" is **not** a remote process or separate runtime: it is a **concurrent in-proc subturn** spawned by a parent requester session and driven by the same LLM+tools loop (async LLM + async tool callbacks).

## What we have today

- A `subagents` tool for orchestration: `spawn | join | cancel | list | steer`.
- Subagent profiles loaded from `config.json` under `agents.subagents`.
- Each profile can load a `SYSTEM.md` prompt from the workspace.
- Each profile can define a tool allowlist, which is converted into a filtered `tools_json` schema passed to the LLM.
- Strong session ownership: a requester session can only control its own subagents.
- Subagents cannot spawn subagents (tool calls from a subagent are forbidden from spawning).

## Feature flags

In `main/mimi_config.h`:

- `MIMI_ENABLE_SUBAGENT`: master switch for subagent framework.
- `MIMI_ENABLE_TOOL_SUBAGENT`: enables the `subagents` tool (defaults to `MIMI_ENABLE_SUBAGENT`).

## Runtime enable/disable switch

Even when compiled in, you can disable subagents at runtime via `config.json`:

```json
{
  "agents": {
    "defaults": {
      "subagentsEnabled": false
    }
  }
}
```

When `subagentsEnabled` is `false`, no profiles will be loaded and the tool will return errors when spawning.

## Default Configuration

The system automatically injects a default subagent profile if `agents.subagents` is missing or empty:

```json
{
  "agents": {
    "defaults": {
      "subagentsEnabled": true
    },
    "subagents": [
      {
        "tools": ["read_file", "write_file", "list_dir", "exec"],
        "maxIters": 20,
        "timeoutSec": 600,
        "isolatedContext": true
      }
    ]
  }
}
```

## Configuration (`config.json`)

Add profiles under `agents.subagents`:

```json
{
  "agents": {
    "defaults": { "defaultMaxIters": 40 },
    "subagents": [
      {
        "name": "File Analyzer",
        "profile": "file_analyzer",
        "systemPromptFile": "agents/file_analyzer/SYSTEM.md",
        "tools": ["read_file", "write_file", "list_dir", "exec"],
        "maxIters": 20,
        "timeoutSec": 600,
        "isolatedContext": true
      }
    ]
  }
}
```

### Fields

- `name`: human readable display name (optional).
- `profile`: lookup key used by `subagents.spawn.profile` (optional, defaults to "default").
- `systemPromptFile`: path to SYSTEM prompt file (optional, defaults to "agents/default/SYSTEM.md").
- `tools`: tool allowlist (array of tool names).
- `maxIters`: iteration cap for the subagent (0 = use profile default).
- `timeoutSec`: wall-clock timeout (best-effort, 0 = use profile default).
- `isolatedContext`: currently advisory; subagents are spawned with a dedicated message history built from `task + context`.

## Tool: `subagents`

### Schema

```json
{
  "type": "object",
  "properties": {
    "action": {
      "type": "string",
      "enum": ["spawn", "join", "cancel", "list", "steer"],
      "default": "list"
    },
    "id": { "type": "string", "description": "Subagent id (for join/cancel/steer)" },
    "mode": {
      "type": "string",
      "enum": ["cancel", "kill"],
      "default": "cancel"
    },
    "waitMs": { "type": "integer", "minimum": 0, "default": 0 },
    "recentMinutes": { "type": "integer", "minimum": 1 },
    "message": { "type": "string" },
    "task": { "type": "string" },
    "context": { "type": "string" },
    "maxIters": { "type": "integer", "minimum": 1 },
    "timeoutSec": { "type": "integer", "minimum": 1 },
    "tools": {
      "type": "string",
      "description": "Optional allowlist override as CSV. Use 'exec' for one-shot commands and 'process' for session control."
    }
  },
  "required": [],
  "additionalProperties": false
}
```

### Actions

- `spawn`: create a new running subagent.
- `join`: wait (optionally) for completion and fetch the current result/excerpt.
- `cancel`: cancel or kill a subagent (or all subagents in the requester session).
- `list`: list subagents owned by the requester session (optionally by recent activity).
- `steer`: enqueue a control message that will be injected into the next iteration.

### Spawn Specification (`subagent_spawn_spec_t`)

```c
typedef struct {
    char profile[64];        /* lookup key used by implementation to select configuration */
    char task[4096];         /* Required. The subagent's primary task/instruction. */
    char context[4096];      /* Optional extra context appended/packaged with initial prompt. */
    int max_iters;           /* 0 => use profile default */
    int timeout_sec;         /* 0 => use profile default */
    bool isolated_context;   /* if false, may inherit parent history in future */
    char tools_csv[256];     /* optional override allowlist (comma-separated) */
} subagent_spawn_spec_t;
```

### Subagent States

| State | Value | Description |
|-------|-------|-------------|
| `SUBAGENT_STATE_PENDING` | 0 | Subagent is queued and not yet started |
| `SUBAGENT_STATE_RUNNING` | 1 | Subagent is actively executing |
| `SUBAGENT_STATE_FINISHED` | 2 | Subagent has completed (success/failure/timeout) |

### Terminal Reasons

| Reason | Value | Description |
|--------|-------|-------------|
| `SUBAGENT_REASON_NONE` | 0 | Not in terminal state |
| `SUBAGENT_REASON_COMPLETED` | 1 | Completed successfully |
| `SUBAGENT_REASON_FAILED` | 2 | Failed with error |
| `SUBAGENT_REASON_CANCELLED` | 3 | Cancelled by user |
| `SUBAGENT_REASON_TIMED_OUT` | 4 | Timed out |
| `SUBAGENT_REASON_KILLED` | 5 | Forcefully killed |
| `SUBAGENT_REASON_CRASHED` | 6 | Crashed |
| `SUBAGENT_REASON_RESOURCE_EXHAUSTED` | 7 | Resource limits exceeded |

### Cancel Modes

| Mode | Value | Description |
|------|-------|-------------|
| `SUBAGENT_CANCEL_SOFT` | 0 | Attempt graceful stop |
| `SUBAGENT_CANCEL_KILL` | 1 | Strong best-effort termination |

### Examples

Spawn:

```json
{
  "action": "spawn",
  "task": "Analyze this project architecture",
  "context": "Optional extra context...",
  "profile": "default",
  "maxIters": 20,
  "timeoutSec": 600
}
```

Join:

```json
{
  "action": "join",
  "id": "sa_...",
  "waitMs": 1000
}
```

List:

```json
{
  "action": "list",
  "recentMinutes": 30
}
```

Cancel:

```json
{
  "action": "cancel",
  "id": "all",
  "mode": "cancel"
}
```

Steer:

```json
{
  "action": "steer",
  "id": "sa_...",
  "message": "Please focus on main/agent/agent_async_loop.c only"
}
```

## Core API

### Manager Initialization

```c
// Initialize the subagent manager (call after config and tool registry are ready)
mimi_err_t subagent_manager_init(void);

// Deinitialize manager and tear down any running tasks
void subagent_manager_deinit(void);
```

### Spawn

```c
mimi_err_t subagent_spawn(const subagent_spawn_spec_t *spec,
                          char *out_id, size_t out_id_size,
                          const mimi_session_ctx_t *parent_ctx);
```

Spawn a new in-proc subagent task.

**Returns**:
- `MIMI_OK` on success
- `MIMI_ERR_PERMISSION_DENIED` if parent_ctx indicates caller is already a subagent
- `MIMI_ERR_NOT_FOUND` if the requested profile cannot be resolved

### Join

```c
mimi_err_t subagent_join(const char *id, int wait_ms,
                         subagent_join_result_t *out,
                         const mimi_session_ctx_t *caller_ctx);
```

Join (observe) a subagent by id.

Waits up to wait_ms milliseconds for completion. If wait_ms <= 0, returns immediately with the current snapshot.

**Access is restricted to the same requester session as the subagent.**

### Cancel

```c
mimi_err_t subagent_cancel(const char *id, subagent_cancel_mode_t mode,
                           int *out_count,
                           const mimi_session_ctx_t *caller_ctx);
```

Cancel a subagent by id, or all subagents owned by the caller if id == "all" or id == "*".

**Parameters**:
- `mode`: Soft cancel attempts graceful stop; kill is stronger best-effort termination
- `out_count`: Optional output for number of tasks affected

### Steer

```c
mimi_err_t subagent_steer(const char *id, const char *message,
                          int *out_queue_depth,
                          const mimi_session_ctx_t *caller_ctx);
```

Steer a running subagent by enqueueing a control message.

The message will be injected into the next LLM iteration. Queue depth is bounded.

### List

```c
mimi_err_t subagent_list(int recent_minutes,
                         char *out_json, size_t out_json_size,
                         const mimi_session_ctx_t *caller_ctx);
```

List subagents owned by the caller's requester session.

**Parameters**:
- `recent_minutes`: If > 0, filters to items updated within the last N minutes
- `out_json`: Output JSON buffer (object with "items" array)

## Configuration API

```c
// Initialize and load profiles from global config + filesystem
mimi_err_t subagent_config_init(void);

// Look up a profile by key
const subagent_profile_runtime_t *subagent_profile_get(const char *profile);

// Release any owned allocations
void subagent_config_deinit(void);
```

## Error Handling

The subagent implementation includes comprehensive error handling:

- **Invalid arguments**: Returns `MIMI_ERR_INVALID_ARG`
- **Permission denied**: Returns `MIMI_ERR_PERMISSION_DENIED` (subagent cannot spawn subagents)
- **Not found**: Returns `MIMI_ERR_NOT_FOUND` (profile not found)
- **Timeout**: Returns appropriate timeout errors
- **Resource exhausted**: Returns `MIMI_ERR_NO_MEM` or similar

## Architecture Benefits

- **Strong Ownership**: Callers can only control their own subagents
- **Concurrent Execution**: Subagents run concurrently with the parent
- **Resource Isolation**: Each subagent has its own message history
- **Tool Filtering**: Tools are filtered based on profile allowlist
- **Graceful Degradation**: Comprehensive error handling and recovery

## Directory Structure

```
main/agent/
  ├── subagent/
  │   ├── subagent_config.c/.h    # Profile configuration loading
  │   ├── subagent_manager.c/.h   # Lifecycle & access control
  │   ├── subagent_task.c/.h      # Execution (LLM loop + async tools)
  │   └── subagent_task.h         # Task state management
  └── tools/
      ├── tool_subagents.c/.h     # `subagents` tool implementation
```

## Lifecycle

1. **Initialization**: `subagent_config_init()` loads profiles, `subagent_manager_init()` initializes manager
2. **Spawn**: Parent calls `subagent_spawn()` with specification
3. **Execution**: Subagent runs async LLM loop with async tool callbacks
4. **Observation**: Parent may `join` to get results or `list` to see status
5. **Control**: Parent may `steer` to inject control messages
6. **Termination**: Subagent completes normally or is `cancel`ed
7. **Cleanup**: Resources are automatically released on completion

## Limitations

- Subagents cannot spawn other subagents (enforced via permission check)
- Tool calls from subagents are filtered by profile allowlist
- Queue depth for steering messages is bounded
- Timeouts are best-effort (not hard real-time)
- Context isolation is currently advisory (full isolation planned)
