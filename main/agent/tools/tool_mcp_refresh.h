#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stddef.h>

const char *tool_mcp_refresh_schema_json(void);
const char *tool_mcp_refresh_description(void);

mimi_err_t tool_mcp_refresh_execute(const char *input_json, char *output, size_t output_size,
                                      const mimi_session_ctx_t *session_ctx);

