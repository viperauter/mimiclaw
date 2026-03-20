# Context Budget / Plan / Assemble (Hook Framework)

This document describes how the `agent` builds the “context” for a single LLM request: what is implemented now, what the interfaces look like, and where to plug in `summary/compact` next.

## Goals

Turn the whole package of `system prompt + history + tools` into a controllable pipeline, instead of being fixed to:
- Always fetching history by `memoryWindow`
- Always composing the system prompt in a fixed way
- Not knowing what to do when the budget is full

So we decompose the “history -> messages” assembly into 3 layers (the system prompt is built outside the assembler by `context_builder`, then passed in):
1. `context_budget` (implemented in `context_budget_plan.c/.h`): budget estimation (currently a chars approximation, not token-accurate)
2. `context_plan` (implemented in `context_budget_plan.c/.h`): choose an initial history window (`memory_window`) based on the budget
3. `context_assembler`: execute assembly (load history, append the current user, trigger hooks, trim to budget, and retry parsing when needed)

## Config Options (Defaults Schema v2)
- `agents.defaults.contextTokens` (int)
  - When > 0, we apply a best-effort token->chars approximation using a simple ASCII vs non-ASCII heuristic.
  - When = 0, we fall back to the previous chars-based approximation derived from buffer sizes.
- `agents.defaults.compaction.model` (string)
  - When empty, compaction/flush trigger uses `thresholdRatio = 1.0` (keeps old behavior).
- `agents.defaults.compaction.memoryFlush.thresholdRatio` (double, 0..1)
  - Controls when the assembler starts trimming earlier to prepare `compact_source_messages`.
  - `0.75` means “trim down to 75% of history budget”.

`context_compact` provides the interfaces and helper functions for compact/summary integration: summary insertion and failure merge.

Hooks exist to insert later strategies (compression/redaction/summary injection) without changing the core assembly logic.

## Directory Structure (agent-side context module)

- `main/agent/context/`
  - `context_builder.c/.h`: build the system prompt (from files + memory)
  - `context_budget_plan.c/.h`: budget estimation + initial memory window selection (heuristics)
  - `context_assembler.c/.h`: assemble messages and do trim/retry
  - `context_compact.c/.h`: summary/compact integration interfaces (currently mainly provides the summary insertion helper)
  - `context_hook.h`: define hook phase interfaces

Top-level include paths retain shims for backward compatibility.

## Assembly Flow for One Request

The current execution flow of `context_assembler` is:

```text
context_assemble_messages_budgeted()
  -> context_budget_compute()
  -> context_plan_choose_initial_memory_window()
  -> for attempt in [0..max_retries)
       -> hooks.pre_history_load() (can modify memory_window)
       -> session_get_history_json(history_json_buf, memory_window)
       -> cJSON_Parse(history_json_buf)
         -> if failed: hooks.on_parse_error(), retry with memory_window halved
       -> hooks.post_history_parsed() (history parsed into an array)
       -> session_append("user") (persist user into history)
       -> append the current user message to `messages`
       -> hooks.post_user_appended()
       -> hooks.pre_trim()
       -> trim_messages_to_budget()
       -> hooks.post_trim()
       -> return messages
  -> fallback: keep only the current user
```

## Hooks: Phases and Trigger Points

Hook contract:
- Hooks must NOT initiate LLM/network requests.
- Hooks may modify:
  - `system_prompt_buf` (when provided in the function signature)
  - `messages` (a cJSON array containing history + the current user)
- If a hook returns an error, the assembler logs it and continues with the default strategy.

`context_hooks_t` (defined in `main/agent/context/context_hook.h`) includes the following optional callbacks:

1. `on_budget`
- Trigger: after `context_budget_compute()` completes
- Use: let hooks observe the budget (future: decide strategies based on it)

2. `pre_history_load`
- Trigger: before each retry attempt calls `session_get_history_json()`
- Use: allow hooks to rewrite this attempt’s `memory_window`

3. `post_history_parsed`
- Trigger: after history JSON is parsed successfully (before appending the current user)
- Use: redaction/replacement for some history segments; prepare preprocessing for summary injection

4. `post_user_appended`
- Trigger: after the current user message is appended into `messages`
- Use: if you need to reorder/inject using the context that includes the current input, do it here

5. `pre_trim`
- Trigger: before trimming
- Use: future compact/summary injection can happen here first (runs before LLM call)

6. `post_trim`
- Trigger: after trimming
- Use: final validation, attach metadata, or do one last trim/reorder

7. `on_parse_error`
- Trigger: history JSON cannot be parsed into an array (usually truncated buffer)
- Use: alerting/metrics, or a smarter degradation strategy in the future

## What’s Done

1. Created `main/agent/context/` to centralize context-related code.
2. Implemented the complete chain of budget estimation + memory-window planning + `context_assembler`.
3. Added during assembly:
   - Retry when history JSON parsing fails (halve `memory_window`)
   - Trim `messages` based on budget; the oldest evicted messages are preserved in `trimmed_messages_for_compact` for compact/summary input.
4. In `agent_async_loop`, replaced the old "fixed history fetch + manual user append" with a call to `context_assemble_messages_budgeted()`.
5. Added the hook interface and implemented hook call points across all phases (hooks default to empty).
6. Implemented LLM-based compact/summary integration: async LLM call on evicted messages, summary insertion at index 0, and failure merge fallback.

## What’s Not Yet Done

1. Token-level budget
- Current budget is estimated in chars, not token-accurate.
- Future: incorporate model context window and token counting/estimation.

2. More fine-grained budget strategy
- The current ratios between tools budget and system budget are simple approximations.
- Future: adjust dynamically based on model size / number of tools.

## Compact/Summary Integration (How It Works)

The LLM-based compact/summary is already implemented. The integration flow is:

1. `context_assemble_messages_budgeted()` calls `trim_messages_to_budget()`, which detaches the oldest messages from `messages` into `trimmed_messages_for_compact` when the budget is exceeded.
2. `agent_async_loop` receives `ctx->compact_source_messages` (= `trimmed_messages_for_compact`) and checks if it is non-empty.
3. If non-empty, it calls `context_compact_maybe_summarize_async()` to invoke an LLM summarizing the evicted messages into a `summary_message`.
4. On success: `context_compact_insert_summary_message()` inserts the summary as a `system` message at index 0 of `ctx->messages`.
5. On failure: `context_compact_merge_compact_source_to_messages()` merges `compact_source_messages` back to the front of `ctx->messages`, preserving all conversation content.

Key files:
- `context_compact.c/.h`: helper functions for insertion and failure merge
- `agent_async_loop.c:59`: `context_compact_summary_callback()` handles async LLM response
- `agent_async_loop.c:129`: `context_compact_maybe_summarize_async()` triggers async compact

## Key Interface Quick Reference

- Main assembly entry:
  - `context_assemble_messages_budgeted(const context_assemble_request_t *req, context_assemble_result_t *out)`
  - When trimming happens, it detaches the oldest trimmed history messages and returns them to the caller (for compact/summary input).
- Hook definition:
  - `context_hooks_t`
- Plan results (for debugging):
  - `context_plan_t.memory_window_used`
  - `context_plan_t.parse_retries`
  - `context_plan_t.did_trim_messages`
  - `context_plan_t.did_run_hook`

## Complete Call Chain (from agent_async_loop to compact)

The following trace shows the full execution path when a user message triggers context assembly and potential compaction:

```text
agent_async_loop_task()                              [agent_async_loop.c:1146]
  │
  ├─► agent_process_user_turn()                     [agent_async_loop.c:1121]
  │       │
  │       ├─► agent_build_system_prompt_for_turn()      [agent_async_loop.c:935]
  │       │       │
  │       │       ├─► context_build_system_prompt()     // Build static system prompt
  │       │       │       (from config files, memory, skills, personality, user info)
  │       │       │
  │       │       └─► strncat(turn_context)             // Inject dynamic turn metadata:
  │       │               "# source_channel: %s\n# source_chat_id: %s\n"
  │       │               (current channel/chat identity for this turn)
  │       │
  │       ├─► agent_assemble_turn_context()         [agent_async_loop.c:953]
  │       │       │
  │       │       └─► context_assemble_messages_budgeted()   [context_assembler.c:243]
  │       │               │
  │       │               ├─► context_budget_compute()               // Compute total and history budgets
  │       │               ├─► context_plan_choose_initial_memory_window()
  │       │               │
  │       │               ├─► for attempt in [0..max_retries)
  │       │               │       │
  │       │               │       ├─► session_get_history_json()    // Fetch session history JSON
  │       │               │       │       ↓
  │       │               │       │   [history_json_buf]              ◄── Raw history JSON string
  │       │               │       │
  │       │               │       ├─► cJSON_Parse(history_json_buf)   // Parse into cJSON array
  │       │               │       │
  │       │               │       ├─► cJSON_AddItemToArray(user_msg)  // Append current user message
  │       │               │       │
  │       │               │       └─► trim_messages_to_budget()     ◄── Key trimming point
  │       │               │               │
  │       │               │               ├─► while (sum > flush_budget_chars && n > 1)
  │       │               │               │       cJSON_DetachItemFromArray(messages, 0)
  │       │               │               │       ↓
  │       │               │               │   [trimmed_for_compact]  ◄── Evicted oldest messages
  │       │               │               │
  │       │               │               └─► out->trimmed_messages_for_compact
  │       │               │                       = trimmed_for_compact
  │       │               │
  │       │               └─► return ctx->messages + ctx->compact_source_messages
  │       │
  │       └─► context_compact_maybe_summarize_async()  [agent_async_loop.c:129]
  │               │
  │               └─► if ctx->compact_source_messages is non-empty:
  │                       │
  │                       ├─► llm_chat_tools_async_req()
  │                       │       (call LLM on compact_source_messages to generate summary)
  │                       │
  │                       └─► context_compact_summary_callback()     [agent_async_loop.c:59]
  │                               │
  │                               ├─► success: context_compact_insert_summary_message()
  │                               │       (insert summary as system message at messages[0])
  │                               │
  │                               └─► failure: context_compact_merge_compact_source_to_messages()
  │                                       (merge compact_source_messages back to messages head)
  │
  └─► agent_start_main_llm_async()  or  triggered after compact callback
```

## What Becomes compact_source_messages

When `trim_messages_to_budget()` is called, it walks the `messages` array from index 0 (oldest) upward and `cJSON_DetachItemFromArray()` any message that causes the total content length to exceed `flush_budget_chars`:

```c
flush_budget_chars = budget.history_budget_chars * memory_flush_threshold_ratio
// thresholdRatio = 1.0 (default): no early trimming, uses full history budget
// thresholdRatio = 0.75: starts trimming when history exceeds 75% of budget
```

- **`messages` array after trim**: contains the newest conversation history that fits within the budget (this is what the main LLM sees).
- **`compact_source_messages`**: contains the oldest messages that were evicted from `messages`. This is the raw input passed to the LLM compact/summary step.

## Session History Files

The conversation history for each channel/chat is stored in **session files** managed by `session_mgr`:

- **Path**: `sessions/{channel}_{chat_id}.jsonl` (JSONL format, one JSON object per line)
- **File format**: `{"role":"user|assistant|tool","content":"...","ts":1234567890}`
- **Managed by**: `main/memory/session_mgr.c`

**Read path** (`session_get_history_json()`):
- Called in `context_assemble_messages_budgeted()` with the chosen `memory_window`
- Reads the session file, uses a ring buffer to keep only the last `memory_window` messages
- Returns a JSON array string `history_json_buf`

**Write path** (`session_append()`):
- Called in `context_assemble_messages_budgeted()` after parsing, before trimming
- Appends the current user message as a new line to the session file (persists the turn)

**Separation of concerns:**
- `session_get_history_json()` + `trim_messages_to_budget()` = builds the `messages` array (conversation history for LLM)
- `context_build_system_prompt()` = static system instructions from files + memory
- `turn_context` = per-turn channel/chat identity metadata
- Session files = durable storage of conversation history across turns

## Data Structures Involved

| Field in `agent_request_ctx_t` | Source | Purpose |
|-------------------------------|--------|---------|
| `messages` | `context_assemble_messages_budgeted()` return | Final message array for main LLM |
| `compact_source_messages` | `trim_messages_to_budget()` via `out->trimmed_messages_for_compact` | Oldest evicted messages → input for compact/summary LLM |
| `history_json_buf` | `session_get_history_json()` | Raw JSON string of session history |
| `system_prompt` | `agent_build_system_prompt_for_turn()` | System prompt built from files + memory |

## System Prompt Structure

The `system_prompt` field is built in two stages:

**Stage 1 — Static part** (`context_build_system_prompt()`, [context_builder.c](file:///home/aotao/workspace/mimiclaw/main/agent/context/context_builder.c)):
- Built once per turn from configuration files and memory
- Includes: agent guidelines, memory file paths, long-term memory contents, recent notes, available skills, personality ("soul"), user info
- This is NOT the conversation history — it is static metadata and instructions

**Stage 2 — Dynamic injection** (`agent_build_system_prompt_for_turn()`, [agent_async_loop.c:935](file:///home/aotao/workspace/mimiclaw/main/agent/agent_async_loop.c#L935)):
- Appends a `turn_context` block to `system_prompt`:
  ```
  ## Current Turn Context
  - source_channel: <msg->channel>
  - source_chat_id: <msg->chat_id>
  ```
- This provides the LLM with the identity of the current message source (e.g., which channel/chat the user is talking to)
- It is per-turn dynamic metadata, distinct from the conversation history in `messages`

**Separation of concerns:**
- `system_prompt` (built by `context_build_system_prompt`) = static instructions, memory, skills, personality
- `messages` (assembled by `context_assemble_messages_budgeted`) = the actual conversation history with user/assistant/tool messages
- `turn_context` injected into `system_prompt` = per-turn channel/chat identity metadata

## Failure Handling Flow

```text
compact/summary LLM call fails
        │
        ▼
context_compact_summary_callback()  [agent_async_loop.c:59]
        │
        ├─► context_compact_merge_compact_source_to_messages()
        │       │
        │       └─► 1. Create new merged array
        │           2. Prepend all items from compact_source_messages (oldest first)
        │           3. Append all items from messages (newest)
        │           4. Replace ctx->messages with merged array
        │
        └─► agent_start_main_llm_async(ctx)  // proceed with merged messages
```

This guarantees conversation continuity: even if compression is unavailable, no content is lost.

