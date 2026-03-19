# Subagents (in-proc) — New Orchestration Interface

This document describes the **current in-process subagent orchestration** system.

In this design, a “subagent” is **not** a remote process or separate runtime: it is a **concurrent in-proc subturn** spawned by a parent requester session and driven by the same LLM+tools loop (async LLM + async tool callbacks).

## What we have today

- A `subagents` tool for orchestration: `spawn | join | cancel | list | steer`.
- Subagent profiles loaded from `config.json` under `agents.subagents`.
- Each profile can load a `SYSTEM.md` prompt from the workspace.
- Each profile can define a tool allowlist, which is converted into a filtered `tools_json` schema passed to the LLM.
- Strong session ownership: a requester session can only control its own subagents.
- Subagents cannot spawn subagents (tool calls from a subagent are forbidden from spawning).

## Feature flag

In `main/mimi_config.h`:

- `MIMI_ENABLE_SUBAGENT`: master switch for subagent framework and the `subagents` tool.

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

## Configuration (`config.json`)

Add profiles under `agents.subagents`:

```json
{
  "agents": {
    "defaults": { "defaultMaxIters": 40 },
    "subagents": [
      {
        "tools": ["read_file", "write_file", "list_dir", "bash"],
        "maxIters": 20,
        "timeoutSec": 600,
        "isolatedContext": true
      }
    ]
  }
}
```

### Fields

- `name` / `profile` / `systemPromptFile`: optional for the default profile.
  - If missing, they default to `profile="default"` and `systemPromptFile="agents/default/SYSTEM.md"`.
  - The `subagents` tool schema intentionally does not advertise `profile` to keep the default UX simple; advanced callers may still pass it.
- `tools`: tool allowlist (array of tool names).
- `maxIters`: iteration cap for the subagent.
- `timeoutSec`: wall-clock timeout (best-effort).
- `isolatedContext`: currently advisory; subagents are spawned with a dedicated message history built from `task + context`.

## Tool: `subagents`

### Actions

- `spawn`: create a new running subagent.
- `join`: wait (optionally) for completion and fetch the current result/excerpt.
- `cancel`: cancel or kill a subagent (or all subagents in the requester session).
- `list`: list subagents owned by the requester session (optionally by recent activity).
- `steer`: enqueue a control message that will be injected into the next iteration.

### Examples

Spawn:

```json
{ "action":"spawn", "task":"Analyze this project architecture", "context":"Optional extra context..." }
```

Join:

```json
{ "action":"join", "id":"sa_...", "waitMs": 1000 }
```

List:

```json
{ "action":"list", "recentMinutes": 30 }
```

Cancel:

```json
{ "action":"cancel", "id":"all", "mode":"cancel" }
```

Steer:

```json
{ "action":"steer", "id":"sa_...", "message":"Please focus on main/agent/agent_async_loop.c only" }
```

