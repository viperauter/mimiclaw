#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"

mimi_err_t tool_subagent_run_execute(const char *input_json,
                                     char *output,
                                     size_t output_size,
                                     const mimi_session_ctx_t *session_ctx);

