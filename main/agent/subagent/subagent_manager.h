#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stdbool.h>
#include <stdint.h>

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
    char profile[64];
    char task[4096];
    char context[4096];
    int max_iters;        /* 0 => use profile default */
    int timeout_sec;      /* 0 => use profile default */
    bool isolated_context;/* if false, may inherit parent history in future */
    char tools_csv[256];  /* optional override allowlist */
} subagent_spawn_spec_t;

typedef struct {
    char id[64];
    char requester_session_key[192];
    subagent_state_t state;
    subagent_terminal_reason_t reason;
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
    int iters_used;
    int tool_calls;
    bool truncated;
    char last_error[256];
    char last_excerpt[512];
} subagent_record_t;

typedef struct {
    bool finished;
    subagent_terminal_reason_t reason;
    bool ok;
    bool truncated;
    char content[4096]; /* excerpt, bounded */
    char error[256];
} subagent_join_result_t;

typedef enum {
    SUBAGENT_CANCEL_SOFT = 0,
    SUBAGENT_CANCEL_KILL = 1,
} subagent_cancel_mode_t;

/* Initialize manager; call after config/tool registry init. */
mimi_err_t subagent_manager_init(void);
void subagent_manager_deinit(void);

/* Spawn a subagent task. */
mimi_err_t subagent_spawn(const subagent_spawn_spec_t *spec,
                          char *out_id, size_t out_id_size,
                          const mimi_session_ctx_t *parent_ctx);

/* Wait for completion up to wait_ms; wait_ms<=0 returns immediately. */
mimi_err_t subagent_join(const char *id, int wait_ms,
                         subagent_join_result_t *out,
                         const mimi_session_ctx_t *caller_ctx);

/* Cancel one subagent by id, or all within requester session if id=="all"|"*". */
mimi_err_t subagent_cancel(const char *id, subagent_cancel_mode_t mode,
                           int *out_count,
                           const mimi_session_ctx_t *caller_ctx);

/* Steer a running subagent; message is queued. */
mimi_err_t subagent_steer(const char *id, const char *message,
                          int *out_queue_depth,
                          const mimi_session_ctx_t *caller_ctx);

/* List subagents for requester session. */
mimi_err_t subagent_list(int recent_minutes,
                         char *out_json, size_t out_json_size,
                         const mimi_session_ctx_t *caller_ctx);

const char *subagent_state_name(subagent_state_t s);
const char *subagent_reason_name(subagent_terminal_reason_t r);

