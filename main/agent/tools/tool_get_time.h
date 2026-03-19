#pragma once

#include "mimi_err.h"
#include <stddef.h>

/**
 * Execute get_current_time tool.
 * Fetches current time via HTTP Date header, sets system clock, returns time string.
 */
#include "memory/session_mgr.h"
const char *tool_get_time_schema_json(void);
const char *tool_get_time_description(void);
mimi_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx);
