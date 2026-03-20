#pragma once

#include "mimi_err.h"

#include "cJSON.h"
#include "context_budget_plan.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct context_hook_result {
    bool handled;
    bool did_modify_messages;
    bool did_modify_system;
} context_hook_result_t;

typedef struct context_hooks {
    /* Optional phase hooks for one context assembly request.
     *
     * Contract:
     * - Hooks must NOT invoke LLM/network (no compact/summarize here yet).
     * - Hooks may mutate:
     *   - `system_prompt_buf` (when provided)
     *   - `messages` array
     * - If a hook returns error, assembler logs and continues.
     */
    void *user_ctx;

    /* After context_budget_compute() but before retry/history selection. */
    mimi_err_t (*on_budget)(void *user_ctx,
                             const context_budget_t *budget,
                             context_hook_result_t *out);

    /* Before session_get_history_json() for each retry attempt.
     * Allows hook to change memory_window for this attempt. */
    mimi_err_t (*pre_history_load)(void *user_ctx,
                                    const char *channel,
                                    const char *chat_id,
                                    int base_memory_window,
                                    int *memory_window_inout,
                                    context_hook_result_t *out);

    /* When history JSON parsed successfully (before appending current user message). */
    mimi_err_t (*post_history_parsed)(void *user_ctx,
                                       const char *channel,
                                       const char *chat_id,
                                       char *system_prompt_buf,
                                       size_t system_prompt_buf_size,
                                       cJSON *messages,
                                       context_hook_result_t *out);

    /* After current user message is appended into `messages`. */
    mimi_err_t (*post_user_appended)(void *user_ctx,
                                       const char *channel,
                                       const char *chat_id,
                                       char *system_prompt_buf,
                                       size_t system_prompt_buf_size,
                                       cJSON *messages,
                                       context_hook_result_t *out);

    /* Before trimming step. */
    mimi_err_t (*pre_trim)(void *user_ctx,
                           const char *channel,
                           const char *chat_id,
                           char *system_prompt_buf,
                           size_t system_prompt_buf_size,
                           cJSON *messages,
                           context_hook_result_t *out);

    /* After trimming step. */
    mimi_err_t (*post_trim)(void *user_ctx,
                            const char *channel,
                            const char *chat_id,
                            char *system_prompt_buf,
                            size_t system_prompt_buf_size,
                            cJSON *messages,
                            context_hook_result_t *out);

    /* Called when history JSON cannot be parsed into an array. */
    mimi_err_t (*on_parse_error)(void *user_ctx,
                                  const char *channel,
                                  const char *chat_id,
                                  int attempt,
                                  int memory_window,
                                  const char *history_json_buf,
                                  context_hook_result_t *out);
} context_hooks_t;

