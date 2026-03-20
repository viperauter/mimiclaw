#pragma once

#include "mimi_err.h"

#include <stdbool.h>
#include <stddef.h>

/* Budget estimation for system/tools vs. history/messages.
 *
 * Budget is computed via a chars-based approximation.
 * When `context_tokens` is provided, we apply a best-effort token->chars mapping
 * (still not token-accurate, but stable enough for trim/flush heuristics).
 */
typedef struct context_budget {
    size_t total_budget_chars;   /* Approximate overall prompt chars budget */
    size_t system_len_chars;     /* strlen(system_prompt) */
    size_t tools_len_chars;      /* strlen(tools_json) */
    size_t history_budget_chars; /* How much content budget to keep in history/messages */
} context_budget_t;

mimi_err_t context_budget_compute(const char *system_prompt,
                                   const char *tools_json,
                                   size_t system_buf_size,
                                   size_t history_json_buf_size,
                                   int context_tokens,
                                   context_budget_t *out);

/* Choose the initial history window size (memory_window) based on budget. */
typedef struct context_plan {
    int memory_window_initial;
    int memory_window_used;
    int parse_retries;
    bool did_trim_messages;
    bool did_retry_on_parse;
    bool did_run_hook;

    /* Trim upper bound used to prepare messages for compaction/summary. */
    size_t history_budget_chars_flush;
    /* Effective threshold ratio used by trim (0..1). */
    double memory_flush_threshold_ratio_used;
} context_plan_t;

int context_plan_choose_initial_memory_window(size_t history_budget_chars, int base_memory_window);

