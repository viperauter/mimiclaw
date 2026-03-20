#pragma once

#include "mimi_err.h"

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

#include "context_budget_plan.h"

/* Enable/disable detailed compaction (context compact/summary) trace logging.
 * This is compile-time so it has near-zero overhead when disabled. */
#ifndef MIMI_TRACE_CONTEXT_COMPACT_DETAILS
#define MIMI_TRACE_CONTEXT_COMPACT_DETAILS 1
#endif

/* Interface for future history compact / summary service.
 *
 * Contract (sync placeholder):
 * - Implementations must NOT mutate `messages_to_compact`.
 * - Implementations must return a heap-allocated cJSON object in `out_summary_message`
 *   (caller owns; usually inserted into final messages array).
 */
typedef struct context_compact_service {
    void *user_ctx;

    mimi_err_t (*compact_history_sync)(void *user_ctx,
                                        const char *channel,
                                        const char *chat_id,
                                        const context_budget_t *budget,
                                        const cJSON *messages_to_compact,
                                        cJSON **out_summary_message);
} context_compact_service_t;

/* Helper used by summary integration code (agent side):
 * - Inserts `summary_message` at index 0 into `messages`.
 * - Ownership of `summary_message` transfers to `messages`.
 */
mimi_err_t context_compact_insert_summary_message(cJSON *messages, cJSON *summary_message);

/**
 * Compact/summary failure fallback merge helper.
 *
 * - `compact_source_messages` is expected to contain the oldest trimmed history
 *   (chronological order already preserved by the assembler).
 * - `*messages` is expected to contain the newest main messages.
 * - The function detaches all items from `compact_source_messages` and prepends
 *   them in front of the items from `*messages`, then updates `*messages`
 *   to point to the merged array.
 * - Ownership of the created merged array transfers to the caller.
 *
 * Note: the container object `compact_source_messages` is intentionally not
 * deleted here; production code may still delete it later.
 */
mimi_err_t context_compact_merge_compact_source_to_messages(cJSON **messages,
                                                            cJSON *compact_source_messages);

/* Debug trace helpers for compaction pipeline.
 * All are best-effort: they should never break the main agent flow. */
void context_compact_trace_llm_input_meta(const char *trace_id, const cJSON *compact_source_messages);
void context_compact_trace_failed_debug(const char *trace_id,
                                         const cJSON *main_messages,
                                         const cJSON *compact_source_messages);
void context_compact_trace_summary_output(const char *trace_id, const char *summary_text);
void context_compact_trace_messages_after(const char *trace_id, const cJSON *messages);
void context_compact_trace_messages_after_failure(const char *trace_id, const cJSON *messages);

