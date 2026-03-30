#pragma once

#include <stdbool.h>
#include "mimi_err.h"
#include "mimi_config.h"
#include "cJSON.h"

/**
 * Agent async context structure
 * Used to track state of asynchronous agent processing
 */
typedef struct {
    char system_prompt[MIMI_CONTEXT_BUF_SIZE];
    cJSON *messages;           /* Message history */
    const char *tools_json;     /* Tools JSON schema */
    char channel[MIMI_CHANNEL_NAME_LEN];
    char chat_id[MIMI_CHAT_ID_LEN];
    char trace_id[MIMI_TRACE_ID_LEN]; /* Trace ID for llm_trace correlation */
    char content[MIMI_AGENT_ASYNC_USER_TEXT_SIZE];
    char final_text[MIMI_AGENT_ASYNC_USER_TEXT_SIZE];
    int iteration;              /* Current iteration */
    int max_iters;              /* Maximum iterations */
    bool sent_working_status;    /* Whether working status was sent */
    char tool_output[MIMI_TOOL_OUTPUT_SIZE];
    void *user_data;            /* User data */
    void (*completion_callback)(mimi_err_t result, const char *text, void *user_data);
} agent_async_ctx_t;

/**
 * Initialize agent async context
 *
 * @param ctx               Agent async context
 * @param channel           Channel identifier
 * @param chat_id           Chat ID
 * @param content           User content
 * @param completion_cb     Completion callback
 * @param user_data         User data
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t agent_async_init(agent_async_ctx_t *ctx, const char *channel, const char *chat_id, const char *content, void (*completion_cb)(mimi_err_t, const char *, void *), void *user_data);

/**
 * Start asynchronous agent processing
 *
 * @param ctx               Agent async context
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t agent_async_start(agent_async_ctx_t *ctx);

/**
 * Cleanup agent async context
 *
 * @param ctx               Agent async context
 */
void agent_async_cleanup(agent_async_ctx_t *ctx);
