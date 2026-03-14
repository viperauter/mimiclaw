#pragma once

#include "mimi_err.h"
#include <stddef.h>

/**
 * Initialize web search tool.
 */
mimi_err_t tool_web_search_init(void);

/**
 * Execute a web search.
 *
 * @param input_json   JSON string with "query" field
 * @param output       Output buffer for formatted search results
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
#include "memory/session_mgr.h"
mimi_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size,
                                  const mimi_session_ctx_t *session_ctx);

/**
 * Save Brave Search API key to NVS.
 */
mimi_err_t tool_web_search_set_key(const char *api_key);
