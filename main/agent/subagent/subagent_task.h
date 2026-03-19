#pragma once

#include "agent/subagent/subagent_manager.h"
#include "agent/subagent/subagent_config.h"
#include "services/llm/llm_proxy.h"
#include "tools/tool_registry.h"
#include "os/os.h"
#include "cJSON.h"

/**
 * Subagent task (execution) API.
 *
 * A subagent task runs an asynchronous loop:
 *   LLM -> (0..N tool calls dispatched asynchronously) -> tool results appended -> next LLM ...
 * until completion, cancellation, timeout, or resource exhaustion.
 *
 * This layer is "per-task". Lifecycle / ownership / routing are handled by `subagent_manager.*`.
 */

typedef struct subagent_task subagent_task_t;

/**
 * Called once when the task transitions into a terminal state.
 * The callback is invoked by the task execution context; keep it lightweight.
 */
typedef void (*subagent_task_on_finish_fn)(subagent_task_t *task, void *user_data);

/**
 * Create a new subagent task object (does not start execution).
 *
 * @param id         Subagent id (assigned by manager; must be stable for join/cancel/steer).
 * @param spec       Spawn specification (task/context + overrides). Must not be NULL.
 * @param profile    Resolved runtime profile (system prompt, tools schema/allowlist, defaults).
 *                  Current implementation expects non-NULL.
 * @param parent_ctx Parent session context; used to derive child ctx and enforce ownership constraints.
 * @param on_finish  Optional completion callback.
 * @param user_data  Opaque pointer passed back to on_finish.
 *
 * @return Newly allocated task on success, NULL on allocation/validation failure.
 */
subagent_task_t *subagent_task_create(const char *id,
                                      const subagent_spawn_spec_t *spec,
                                      const subagent_profile_runtime_t *profile,
                                      const mimi_session_ctx_t *parent_ctx,
                                      subagent_task_on_finish_fn on_finish,
                                      void *user_data);

/** Destroy a task and release all owned resources. */
void subagent_task_destroy(subagent_task_t *t);

/**
 * Start task execution (asynchronous).
 * On success the task enters RUNNING and schedules its first LLM call.
 */
mimi_err_t subagent_task_start(subagent_task_t *t);

/**
 * Request cancellation of a running task.
 *
 * Soft cancel attempts best-effort graceful termination; kill requests stronger termination.
 * Actual stopping is checked at key points (before/after LLM callbacks and tool callbacks).
 */
void subagent_task_cancel(subagent_task_t *t, subagent_cancel_mode_t mode);

/**
 * Enqueue a steering message for a running task.
 *
 * The message is injected into the next LLM iteration. Queue depth is bounded.
 *
 * @param out_depth Optional output for the current steering queue depth.
 */
mimi_err_t subagent_task_steer(subagent_task_t *t, const char *msg, int *out_depth);

/** Copy the current lightweight record snapshot (for list/debug). Thread-safe. */
void subagent_task_snapshot(subagent_task_t *t, subagent_record_t *out_rec);

/** Returns true if the task has finished (terminal state). Thread-safe. */
bool subagent_task_is_finished(subagent_task_t *t);

/** Copy the current join result (excerpt + best-effort final_text). Thread-safe. */
void subagent_task_join_result(subagent_task_t *t, subagent_join_result_t *out);

/**
 * Wait for completion up to wait_ms milliseconds (best-effort).
 *
 * @return true if finished before the timeout, false otherwise.
 */
bool subagent_task_wait(subagent_task_t *t, int wait_ms);

