#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stddef.h>

const char *tool_exec_schema_json(void);
const char *tool_exec_description(void);

typedef enum {
    TOOL_EXEC_ACT_POLL = 0,
    TOOL_EXEC_ACT_SEND = 1,
    TOOL_EXEC_ACT_KILL = 2,
} tool_exec_action_t;

mimi_err_t tool_exec_process_attach(const char *session_id,
                                   char *output, size_t output_size,
                                   tool_exec_action_t action,
                                   const char *text,
                                   size_t cursor,
                                   int wait_ms,
                                   int max_bytes);

mimi_err_t tool_exec_execute(const char *input_json, char *output, size_t output_size,
                            const mimi_session_ctx_t *session_ctx);

