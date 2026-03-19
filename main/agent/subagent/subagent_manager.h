#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Subagents (in-proc) manager API.
 *
 * This module owns the lifecycle and access control of running subagent tasks:
 * - Spawn / join / list / steer / cancel
 * - Enforces requester-session ownership (callers can only control their own subagents)
 * - Provides stable snapshots (`subagent_record_t`) and results (`subagent_join_result_t`)
 *
 * Execution (LLM loop + async tools) is implemented in `subagent_task.*`.
 */

typedef enum {
    SUBAGENT_STATE_PENDING = 0,
    SUBAGENT_STATE_RUNNING,
    SUBAGENT_STATE_FINISHED,
} subagent_state_t;

typedef enum {
    SUBAGENT_REASON_NONE = 0,
    SUBAGENT_REASON_COMPLETED,
    SUBAGENT_REASON_FAILED,
    SUBAGENT_REASON_CANCELLED,
    SUBAGENT_REASON_TIMED_OUT,
    SUBAGENT_REASON_KILLED,
    SUBAGENT_REASON_CRASHED,
    SUBAGENT_REASON_RESOURCE_EXHAUSTED,
} subagent_terminal_reason_t;

typedef struct {
    /**
     * Profile lookup key used by the implementation to select configuration
     * (system prompt, tool allowlist, defaults). The `subagents` tool defaults
     * this to "default" when omitted.
     */
    char profile[64];
    /** Required. The subagent's primary task/instruction. */
    char task[4096];
    /** Optional extra context appended/packaged with the initial prompt. */
    char context[4096];
    int max_iters;        /* 0 => use profile default */
    int timeout_sec;      /* 0 => use profile default */
    bool isolated_context;/* if false, may inherit parent history in future */
    char tools_csv[256];  /* optional override allowlist */
} subagent_spawn_spec_t;

typedef struct {
    /** Subagent id (stable handle used by join/cancel/steer). */
    char id[64];
    /** Ownership boundary: only callers with the same requester_session_key may control it. */
    char requester_session_key[192];
    subagent_state_t state;
    subagent_terminal_reason_t reason;
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
    /** LLM iterations consumed so far. */
    int iters_used;
    /** Total tool calls issued by the subagent so far. */
    int tool_calls;
    /** True when output was truncated to fit bounded buffers. */
    bool truncated;
    /** Short last error string (for list/debug). */
    char last_error[256];
    /** Short last excerpt (for list/debug). */
    char last_excerpt[512];
} subagent_record_t;

typedef struct {
    /** Whether the subagent has reached a terminal state. */
    bool finished;
    subagent_terminal_reason_t reason;
    /** Convenience success flag (true for normal completion). */
    bool ok;
    /** True when either excerpt or final_text was truncated to buffer limits. */
    bool truncated;
    /** A short excerpt suitable for quick display. */
    char content[4096];
    /** Best-effort final response text (bounded). Prefer this for orchestration/merge/review. */
    char final_text[32768];
    /** Error message when ok==false (bounded). */
    char error[256];
} subagent_join_result_t;

typedef enum {
    SUBAGENT_CANCEL_SOFT = 0,
    SUBAGENT_CANCEL_KILL = 1,
} subagent_cancel_mode_t;

/**
 * Initialize the subagent manager.
 *
 * Must be called after global config and tool registry are ready.
 * Safe to call multiple times (subsequent calls are no-ops).
 */
mimi_err_t subagent_manager_init(void);

/** Deinitialize manager and tear down any running tasks. */
void subagent_manager_deinit(void);

/**
 * Spawn a new in-proc subagent task.
 *
 * @param spec        Spawn specification (task/context + overrides). `spec->task` is required.
 * @param out_id      Output buffer for generated subagent id.
 * @param out_id_size Size of out_id buffer.
 * @param parent_ctx  Caller session context. Used for ownership and to prevent subagents from spawning subagents.
 *
 * @return MIMI_OK on success.
 *         MIMI_ERR_PERMISSION_DENIED if parent_ctx indicates caller is already a subagent.
 *         MIMI_ERR_NOT_FOUND if the requested profile cannot be resolved.
 */
mimi_err_t subagent_spawn(const subagent_spawn_spec_t *spec,
                          char *out_id, size_t out_id_size,
                          const mimi_session_ctx_t *parent_ctx);

/**
 * Join (observe) a subagent by id.
 *
 * Waits up to wait_ms milliseconds for completion. If wait_ms<=0, returns immediately
 * with the current snapshot.
 *
 * Access is restricted to the same requester session as the subagent.
 */
mimi_err_t subagent_join(const char *id, int wait_ms,
                         subagent_join_result_t *out,
                         const mimi_session_ctx_t *caller_ctx);

/**
 * Cancel a subagent by id, or all subagents owned by the caller if id=="all" or id=="*".
 *
 * @param mode Soft cancel attempts graceful stop; kill is stronger best-effort termination.
 * @param out_count Optional output for number of tasks affected.
 */
mimi_err_t subagent_cancel(const char *id, subagent_cancel_mode_t mode,
                           int *out_count,
                           const mimi_session_ctx_t *caller_ctx);

/**
 * Steer a running subagent by enqueueing a control message.
 *
 * The message will be injected into the next LLM iteration. Queue depth is bounded.
 */
mimi_err_t subagent_steer(const char *id, const char *message,
                          int *out_queue_depth,
                          const mimi_session_ctx_t *caller_ctx);

/**
 * List subagents owned by the caller's requester session.
 *
 * @param recent_minutes If >0, filters to items updated within the last N minutes.
 * @param out_json       Output JSON buffer (object with "items" array).
 */
mimi_err_t subagent_list(int recent_minutes,
                         char *out_json, size_t out_json_size,
                         const mimi_session_ctx_t *caller_ctx);

const char *subagent_state_name(subagent_state_t s);
const char *subagent_reason_name(subagent_terminal_reason_t r);

