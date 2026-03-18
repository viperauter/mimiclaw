#pragma once

#include "agent/subagent/subagent_manager.h"
#include "agent/subagent/subagent_config.h"
#include "services/llm/llm_proxy.h"
#include "tools/tool_registry.h"
#include "os/os.h"
#include "cJSON.h"

typedef struct subagent_task subagent_task_t;

typedef void (*subagent_task_on_finish_fn)(subagent_task_t *task, void *user_data);

subagent_task_t *subagent_task_create(const char *id,
                                      const subagent_spawn_spec_t *spec,
                                      const subagent_profile_runtime_t *profile,
                                      const mimi_session_ctx_t *parent_ctx,
                                      subagent_task_on_finish_fn on_finish,
                                      void *user_data);

void subagent_task_destroy(subagent_task_t *t);

/* Starts the task (async). */
mimi_err_t subagent_task_start(subagent_task_t *t);

/* Cancellation and steering. */
void subagent_task_cancel(subagent_task_t *t, subagent_cancel_mode_t mode);
mimi_err_t subagent_task_steer(subagent_task_t *t, const char *msg, int *out_depth);

/* Query */
void subagent_task_snapshot(subagent_task_t *t, subagent_record_t *out_rec);
bool subagent_task_is_finished(subagent_task_t *t);
void subagent_task_join_result(subagent_task_t *t, subagent_join_result_t *out);

/* Wait for completion (best-effort). Returns true if finished. */
bool subagent_task_wait(subagent_task_t *t, int wait_ms);

