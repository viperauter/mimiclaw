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
 * LLM response callback function type
 *
 * @param result        Result code
 * @param resp          LLM response (must be freed by caller)
 * @param user_data     User data passed to llm_chat_tools_async
 */
typedef void (*llm_callback_t)(mimi_err_t result, llm_response_t *resp, void *user_data);

/* ── Request object (preferred API) ───────────────────────────── */

typedef struct {
    const char *system_prompt;  /* optional */
    cJSON *messages;            /* required: cJSON array of messages (caller owns) */
    const char *tools_json;     /* optional: JSON string array of tools */
    const char *trace_id;       /* optional: for llm_trace correlation */
    const char *model_override; /* optional: when non-empty, use this model for this request */
} llm_chat_req_t;

/**
 * Preferred synchronous API: send chat completion with a request object.
 */
mimi_err_t llm_chat_tools_req(const llm_chat_req_t *req, llm_response_t *resp);

/**
 * Preferred asynchronous API: send chat completion with a request object.
 */
mimi_err_t llm_chat_tools_async_req(const llm_chat_req_t *req,
                                   llm_response_t *resp,
                                   llm_callback_t callback,
                                   void *user_data);

/**
 * Get the last error message from the LLM proxy.
 * 
 * @return The last error message, or an empty string if no error.
 */
const char *llm_get_last_error(void);
