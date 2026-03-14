#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;  /* JSON Schema string for input */
    bool requires_confirmation;     /* Whether tool requires user confirmation */
    mimi_err_t (*execute)(const char *input_json, char *output, size_t output_size,
                          const mimi_session_ctx_t *session_ctx);
} mimi_tool_t;

typedef void (*tool_callback_t)(mimi_err_t result, const char *tool_name, const char *output, void *user_data);

mimi_err_t tool_registry_init(void);

const char *tool_registry_get_tools_json(void);

mimi_err_t tool_registry_execute(const char *name, const char *input_json,
                                 char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx);

mimi_err_t tool_registry_execute_async(const char *name, const char *input_json,
                                       const mimi_session_ctx_t *session_ctx,
                                       tool_callback_t callback, void *user_data);

typedef struct {
    const char *name;
    const char *input_json;
} tool_call_t;

mimi_err_t tool_registry_execute_all_async(const tool_call_t *calls, int call_count,
                                           const mimi_session_ctx_t *session_ctx,
                                           tool_callback_t callback, void *user_data);

bool tool_registry_requires_confirmation(const char *tool_name);

mimi_err_t tool_registry_deinit(void);
