#pragma once

#include "agent/subagent/subagent_config.h"
#include "memory/session_mgr.h"
#include <stdbool.h>

typedef struct {
    char task[4096];
    char context[4096];
} subagent_request_t;

typedef struct {
    bool ok;
    char content[8192];
    char error[512];
} subagent_result_t;

/**
 * Run a subagent by role/name using the in-process implementation.
 * Future implementations (fork/tcp) should keep this API stable.
 */
mimi_err_t subagent_run(const char *role,
                        const subagent_request_t *req,
                        subagent_result_t *out,
                        const mimi_session_ctx_t *session_ctx);

