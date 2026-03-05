#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stddef.h>

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;  /* JSON Schema string for input */
    mimi_err_t (*execute)(const char *input_json, char *output, size_t output_size,
                          const mimi_session_ctx_t *session_ctx);
} mimi_tool_t;

/**
 * Initialize tool registry and register all built-in tools.
 */
mimi_err_t tool_registry_init(void);

/**
 * Get the pre-built tools JSON array string for the API request.
 * Returns NULL if no tools are registered.
 */
const char *tool_registry_get_tools_json(void);

/**
 * Execute a tool by name with session context.
 *
 * @param name         Tool name (e.g. "web_search")
 * @param input_json   JSON string of tool input
 * @param output       Output buffer for tool result text
 * @param output_size  Size of output buffer
 * @param session_ctx  Session context (NULL when no session, e.g. CLI tool_exec)
 */
mimi_err_t tool_registry_execute(const char *name, const char *input_json,
                                 char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx);
