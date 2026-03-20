#include "context_budget_plan.h"

#include <string.h>

#include "log.h"

static const char *TAG_BUDGET = "context_budget";
static const char *TAG_PLAN = "context_plan";

mimi_err_t context_budget_compute(const char *system_prompt,
                                   const char *tools_json,
                                   size_t system_buf_size,
                                   size_t history_json_buf_size,
                                   int context_tokens,
                                   context_budget_t *out)
{
    if (!out) return MIMI_ERR_INVALID_ARG;

    size_t sys_len = (system_prompt && system_prompt[0]) ? strlen(system_prompt) : 0;
    size_t tools_len = (tools_json && tools_json[0]) ? strlen(tools_json) : 0;

    size_t overhead = 512;

    size_t used = sys_len + tools_len + overhead;

    size_t total_budget_chars = 0;
    double avg_chars_per_token = 0.0;

    if (context_tokens > 0) {
        /* Best-effort token->chars mapping:
         * ASCII bytes tend to be "more chars per token" than CJK/non-ascii. */
        size_t ascii_bytes = 0;
        size_t non_ascii_bytes = 0;
        size_t total_bytes = 0;

        const unsigned char *p1 = (const unsigned char *)system_prompt;
        if (p1) {
            for (size_t i = 0; p1[i] != '\0'; i++) {
                unsigned char c = p1[i];
                total_bytes++;
                if (c < 0x80) ascii_bytes++;
                else non_ascii_bytes++;
            }
        }
        const unsigned char *p2 = (const unsigned char *)tools_json;
        if (p2) {
            for (size_t i = 0; p2[i] != '\0'; i++) {
                unsigned char c = p2[i];
                total_bytes++;
                if (c < 0x80) ascii_bytes++;
                else non_ascii_bytes++;
            }
        }

        double ascii_ratio = (total_bytes > 0) ? ((double)ascii_bytes / (double)total_bytes) : 1.0;
        double cjk_ratio = 1.0 - ascii_ratio;

        /* Tuneable constants:
         * - ASCII: ~4 chars/token
         * - CJK/non-ascii: ~1.5 chars/token */
        avg_chars_per_token = (ascii_ratio * 4.0) + (cjk_ratio * 1.5);
        if (avg_chars_per_token < 1.0) avg_chars_per_token = 1.0;

        double total_chars = (double)context_tokens * avg_chars_per_token;
        total_budget_chars = (size_t)(total_chars * 0.90); /* keep 10% safety */
    } else {
        size_t total_buf = system_buf_size + history_json_buf_size;
        total_budget_chars = (total_buf * 90) / 100; /* keep 10% safety */
        avg_chars_per_token = 0.0;
    }

    size_t history_budget = (total_budget_chars > used) ? (total_budget_chars - used) : 0;

    out->total_budget_chars = total_budget_chars;
    out->system_len_chars = sys_len;
    out->tools_len_chars = tools_len;
    out->history_budget_chars = history_budget;

    MIMI_LOGD(TAG_BUDGET,
              "budget: context_tokens=%d avg_cpt=%.2f total=%zu system=%zu tools=%zu history=%zu",
              context_tokens,
              avg_chars_per_token,
              out->total_budget_chars,
              out->system_len_chars,
              out->tools_len_chars,
              out->history_budget_chars);

    return MIMI_OK;
}

int context_plan_choose_initial_memory_window(size_t history_budget_chars, int base_memory_window)
{
    int base = (base_memory_window > 0) ? base_memory_window : 20;

    const size_t avg_chars_per_msg = 240;
    size_t can_keep = (avg_chars_per_msg > 0) ? (history_budget_chars / avg_chars_per_msg) : 0;
    int initial = (int)can_keep;

    if (initial < 1) initial = 1;
    if (initial > base) initial = base;

    /* For now: heuristic-only. When token-budget lands, this function becomes smarter. */
    (void)TAG_PLAN;
    return initial;
}

