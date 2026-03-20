#pragma once

#include "mimi_err.h"

#include "context_hook.h"
#include "context_budget_plan.h"

#include "cJSON.h"

#include <stddef.h>

/* Request/Result wrapper to reduce "long parameter list" in callers. */
typedef struct context_assemble_request {
    const char *channel;
    const char *chat_id;
    const char *user_content; /* Optional: can be NULL */

    char *system_prompt_buf;
    size_t system_prompt_buf_size;

    const char *tools_json;
    int base_memory_window;

    /* Optional context window budgeting (token->chars approximation). */
    int context_tokens; /* 0 => fallback to chars-based buffer estimate */

    /* When compaction is enabled, start trimming earlier using this ratio (0..1). */
    double memory_flush_threshold_ratio; /* 1.0 => keep old behavior */

    char *history_json_buf;
    size_t history_json_buf_size;

    const context_hooks_t *hooks; /* Optional */
} context_assemble_request_t;

typedef struct context_assemble_result {
    cJSON *messages; /* Ownership transferred to caller */
    cJSON *trimmed_messages_for_compact; /* Ownership transferred to caller */

    context_plan_t plan;
    context_budget_t budget; /* For trace/debug/traceability */
} context_assemble_result_t;

mimi_err_t context_assemble_messages_budgeted(const context_assemble_request_t *req,
                                               context_assemble_result_t *out);

/* Best-effort debug trace helpers for the split between:
 * - ctx->messages (main LLM input)
 * - ctx->compact_source_messages (compact/summary LLM input) */
void context_assembler_trace_context_split(const char *trace_id,
                                           const cJSON *main_messages,
                                           const cJSON *compact_source_messages);

