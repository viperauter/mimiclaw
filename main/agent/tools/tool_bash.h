#pragma once

#include "mimi_err.h"
#include <stddef.h>

typedef enum {
    TOOL_BASH_ENV_HOST,
    TOOL_BASH_ENV_SANDBOX
} tool_bash_env_t;

#include "memory/session_mgr.h"

mimi_err_t tool_bash_execute(const char *input_json, char *output, size_t output_size,
                             const mimi_session_ctx_t *session_ctx);

void tool_bash_set_env(tool_bash_env_t env);

tool_bash_env_t tool_bash_get_env(void);

void tool_bash_set_workspace(const char *workspace);
