#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    const char *name;
    bool requires_confirmation_default;
    mimi_err_t (*init)(void);
    mimi_err_t (*deinit)(void);
    const char *(*get_tools_json)(void); /* JSON array string */
    mimi_err_t (*execute)(const char *tool_name, const char *input_json,
                          char *output, size_t output_size,
                          const mimi_session_ctx_t *session_ctx);
    bool (*requires_confirmation)(const char *tool_name);
} mimi_tool_provider_t;

mimi_err_t tool_provider_registry_init(void);
void tool_provider_registry_deinit(void);

mimi_err_t tool_provider_register(const mimi_tool_provider_t *provider);
const char *tool_provider_get_all_tools_json(void);

mimi_err_t tool_provider_execute(const char *tool_name, const char *input_json,
                                 char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx);
bool tool_provider_requires_confirmation(const char *tool_name, bool fallback);
