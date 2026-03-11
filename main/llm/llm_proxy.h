#pragma once

#include "mimi_err.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdbool.h>

#define LLM_MAX_TOOL_CALLS 4

/**
 * Initialize the LLM proxy. Reads API key and model from build-time secrets, then NVS.
 */
mimi_err_t llm_proxy_init(void);

/**
 * Save the LLM API key to NVS.
 */
mimi_err_t llm_set_api_key(const char *api_key);

/**
 * Save the LLM provider to NVS. (e.g. "anthropic", "openai")
 */
mimi_err_t llm_set_provider(const char *provider);

/**
 * Save the model identifier to NVS.
 */
mimi_err_t llm_set_model(const char *model);

/* ── Tool Use Support ──────────────────────────────────────────── */

typedef struct {
    char id[64];        /* "toolu_xxx" */
    char name[32];      /* "web_search" */
    char *input;        /* heap-allocated JSON string */
    size_t input_len;
} llm_tool_call_t;

typedef struct {
    char *text;                                  /* accumulated text blocks */
    size_t text_len;
    llm_tool_call_t calls[LLM_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;                               /* stop_reason == "tool_use" */
} llm_response_t;

void llm_response_free(llm_response_t *resp);

/**
 * Send a chat completion request with tools to the configured LLM API (non-streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages       cJSON array of messages (caller owns)
 * @param tools_json     Pre-built JSON string of tools array, or NULL for no tools
 * @param resp           Output: structured response with text and tool calls
 * @return ESP_OK on success
 */
mimi_err_t llm_chat_tools(const char *system_prompt,
                          cJSON *messages,
                          const char *tools_json,
                          llm_response_t *resp);

/**
 * Get the last error message from the LLM proxy.
 * 
 * @return The last error message, or an empty string if no error.
 */
const char *llm_get_last_error(void);
