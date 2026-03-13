#pragma once

#include <stdbool.h>
#include "mimi_err.h"
#include "cJSON.h"

/**
 * Agent async context structure
 * Used to track state of asynchronous agent processing
 */
typedef struct {
    char system_prompt[16384];  /* System prompt */
    cJSON *messages;           /* Message history */
    const char *tools_json;     /* Tools JSON schema */
    char channel[64];           /* Channel identifier */
    char chat_id[64];           /* Chat ID */
    char content[32768];        /* User content */
    char final_text[32768];     /* Final response text */
    int iteration;              /* Current iteration */
    int max_iters;              /* Maximum iterations */
    bool sent_working_status;    /* Whether working status was sent */
    char tool_output[8192];     /* Tool output buffer */
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
