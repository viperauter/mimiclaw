#include "context_assembler.h"

#include <string.h>
#include <stdio.h>

#include "log.h"
#include "llm/llm_trace.h"
#include "memory/session_mgr.h"

static const char *TAG = "context_assembler";

#ifndef MIMI_TRACE_CONTEXT_COMPACT_DETAILS
#define MIMI_TRACE_CONTEXT_COMPACT_DETAILS 1
#endif

#define TRACE_PREVIEW_MAX_BYTES 2048
#define TRACE_PREVIEW_MSGS_MAX 8
#define TRACE_PREVIEW_CONTENT_MAX_CHARS 220

static size_t message_content_len_for_preview(const cJSON *msg)
{
    if (!msg) return 0;
    const cJSON *content = cJSON_GetObjectItem((cJSON *)msg, "content");
    if (!content || !cJSON_IsString((cJSON *)content) || !content->valuestring) return 0;
    return strlen(content->valuestring);
}

static size_t messages_total_content_len_for_preview(const cJSON *messages)
{
    if (!messages || !cJSON_IsArray((cJSON *)messages)) return 0;
    int n = cJSON_GetArraySize((cJSON *)messages);
    size_t sum = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem((cJSON *)messages, i);
        sum += message_content_len_for_preview(it);
    }
    return sum;
}

static void normalize_for_preview(char *dst, size_t dst_sz, const char *src, size_t max_chars)
{
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t n = strnlen(src, max_chars);
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < dst_sz; i++) {
        char c = src[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static void build_messages_preview_for_split(const cJSON *messages,
                                                char *out, size_t out_sz,
                                                size_t max_items,
                                                size_t max_content_chars)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!messages || !cJSON_IsArray((cJSON *)messages)) {
        snprintf(out, out_sz, "<non-array>");
        return;
    }

    int n = cJSON_GetArraySize((cJSON *)messages);
    if (n <= 0) {
        snprintf(out, out_sz, "[]");
        return;
    }

    size_t used = 0;
    size_t items = (size_t)n < max_items ? (size_t)n : max_items;
    for (size_t i = 0; i < items; i++) {
        const cJSON *it = cJSON_GetArrayItem((cJSON *)messages, (int)i);
        const cJSON *role = it ? cJSON_GetObjectItem((cJSON *)it, "role") : NULL;
        const cJSON *content = it ? cJSON_GetObjectItem((cJSON *)it, "content") : NULL;

        const char *role_s = (role && cJSON_IsString((cJSON *)role) && role->valuestring) ? role->valuestring : "?";
        const char *content_s =
            (content && cJSON_IsString((cJSON *)content) && content->valuestring) ? content->valuestring : "";

        size_t clen = strlen(content_s);
        char frag[TRACE_PREVIEW_CONTENT_MAX_CHARS + 16];
        normalize_for_preview(frag, sizeof(frag), content_s, max_content_chars);

        int wrote = snprintf(out + used, (out_sz > used) ? (out_sz - used) : 0,
                             "[%u]%s(%zu)=\"%s\"; ",
                             (unsigned)i, role_s, clen, frag);
        if (wrote < 0) break;
        if ((size_t)wrote >= (out_sz > used ? (out_sz - used) : 0)) break;
        used += (size_t)wrote;
        if (used + 1 >= out_sz) break;
    }

    if ((size_t)n > items) {
        (void)snprintf(out + used, out_sz - used, "...(+%d)", n - (int)items);
    }
}

void context_assembler_trace_context_split(const char *trace_id,
                                           const cJSON *main_messages,
                                           const cJSON *compact_source_messages)
{
    if (!MIMI_TRACE_CONTEXT_COMPACT_DETAILS) return;
    if (!trace_id || !trace_id[0]) return;

    if (!main_messages || !cJSON_IsArray((cJSON *)main_messages)) return;
    if (!compact_source_messages || !cJSON_IsArray((cJSON *)compact_source_messages)) return;

    int main_n = cJSON_GetArraySize((cJSON *)main_messages);
    int compact_n = cJSON_GetArraySize((cJSON *)compact_source_messages);
    size_t main_chars = messages_total_content_len_for_preview(main_messages);
    size_t compact_chars = messages_total_content_len_for_preview(compact_source_messages);

    char main_n_buf[32], compact_n_buf[32];
    char main_chars_buf[32], compact_chars_buf[32];
    snprintf(main_n_buf, sizeof(main_n_buf), "%d", main_n);
    snprintf(compact_n_buf, sizeof(compact_n_buf), "%d", compact_n);
    snprintf(main_chars_buf, sizeof(main_chars_buf), "%zu", main_chars);
    snprintf(compact_chars_buf, sizeof(compact_chars_buf), "%zu", compact_chars);

    llm_trace_event_kv(trace_id, "context_split_meta",
                       "main_count", main_n_buf,
                       "compact_count", compact_n_buf,
                       "main_chars", main_chars_buf,
                       "compact_chars", compact_chars_buf);

    char main_preview[TRACE_PREVIEW_MAX_BYTES + 1];
    char compact_preview[TRACE_PREVIEW_MAX_BYTES + 1];
    build_messages_preview_for_split(main_messages,
                                      main_preview, sizeof(main_preview),
                                      TRACE_PREVIEW_MSGS_MAX, TRACE_PREVIEW_CONTENT_MAX_CHARS);
    build_messages_preview_for_split(compact_source_messages,
                                      compact_preview, sizeof(compact_preview),
                                      TRACE_PREVIEW_MSGS_MAX, TRACE_PREVIEW_CONTENT_MAX_CHARS);

    llm_trace_event_kv(trace_id, "context_split_main_preview",
                       "preview", main_preview,
                       NULL, NULL,
                       NULL, NULL,
                       NULL, NULL);

    llm_trace_event_kv(trace_id, "context_split_compact_preview",
                       "preview", compact_preview,
                       NULL, NULL,
                       NULL, NULL,
                       NULL, NULL);
}

static size_t content_len_for_message(const cJSON *msg)
{
    if (!msg) return 0;
    const cJSON *content = cJSON_GetObjectItem((cJSON *)msg, "content");
    if (!content || !cJSON_IsString(content) || !content->valuestring) return 0;
    return strlen(content->valuestring);
}

static size_t total_content_len(const cJSON *messages)
{
    if (!messages || !cJSON_IsArray((cJSON *)messages)) return 0;
    size_t sum = 0;
    int n = cJSON_GetArraySize((cJSON *)messages);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem((cJSON *)messages, i);
        sum += content_len_for_message(it);
    }
    return sum;
}

static void trim_messages_to_budget(cJSON *messages,
                                    size_t history_budget_chars,
                                    bool *did_trim,
                                    cJSON **out_trimmed_messages)
{
    if (did_trim) *did_trim = false;
    if (out_trimmed_messages) *out_trimmed_messages = NULL;
    if (!messages || !cJSON_IsArray(messages)) return;

    int n_before = cJSON_GetArraySize(messages);
    size_t sum = total_content_len(messages);
    int n = cJSON_GetArraySize(messages);

    cJSON *trimmed = NULL;
    if (out_trimmed_messages) {
        trimmed = cJSON_CreateArray();
        if (trimmed) {
            *out_trimmed_messages = trimmed;
        }
    }

    if (history_budget_chars == 0) {
        while (n > 1) {
            cJSON *first = cJSON_GetArrayItem(messages, 0);
            size_t first_len = content_len_for_message(first);
            cJSON *det = cJSON_DetachItemFromArray(messages, 0);
            if (det) {
                if (trimmed) {
                    cJSON_AddItemToArray(trimmed, det);
                } else {
                    cJSON_Delete(det);
                }
            }
            sum = (first_len > sum) ? 0 : (sum - first_len);
            n = cJSON_GetArraySize(messages);
        }
        if (did_trim && n_before != n) *did_trim = true;
        if (out_trimmed_messages && trimmed && cJSON_GetArraySize(trimmed) == 0) {
            cJSON_Delete(trimmed);
            *out_trimmed_messages = NULL;
        }
        return;
    }

    while (sum > history_budget_chars && n > 1) {
        cJSON *first = cJSON_GetArrayItem(messages, 0);
        size_t first_len = content_len_for_message(first);
        cJSON *det = cJSON_DetachItemFromArray(messages, 0);
        if (det) {
            if (trimmed) {
                cJSON_AddItemToArray(trimmed, det);
            } else {
                cJSON_Delete(det);
            }
        }
        sum = (first_len > sum) ? 0 : (sum - first_len);
        if (sum > history_budget_chars) {
            sum = total_content_len(messages);
        }
        n = cJSON_GetArraySize(messages);
    }

    if (did_trim && n_before != n) *did_trim = true;

    if (out_trimmed_messages && trimmed && cJSON_GetArraySize(trimmed) == 0) {
        cJSON_Delete(trimmed);
        *out_trimmed_messages = NULL;
    }
}

mimi_err_t context_assemble_messages_budgeted(const context_assemble_request_t *req,
                                               context_assemble_result_t *out)
{
    if (!req || !out ||
        !req->history_json_buf || req->history_json_buf_size == 0 ||
        req->history_json_buf_size < 2 ||
        !req->system_prompt_buf || req->system_prompt_buf_size == 0) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!req->channel || !req->channel[0] || !req->chat_id || !req->chat_id[0]) {
        return MIMI_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    /* Compute budget first (chars-based, or token->chars approximation when context_tokens>0). */
    (void)context_budget_compute(req->system_prompt_buf, req->tools_json,
                                 req->system_prompt_buf_size, req->history_json_buf_size,
                                 req->context_tokens, &out->budget);

    /* Compute effective trim upper bound used to prepare compact/summary input. */
    double ratio = req->memory_flush_threshold_ratio;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    size_t flush_budget_chars = (size_t)((double)out->budget.history_budget_chars * ratio);

    context_plan_t plan = {0};
    plan.memory_window_initial = context_plan_choose_initial_memory_window(out->budget.history_budget_chars,
                                                                           req->base_memory_window);
    plan.memory_window_used = plan.memory_window_initial;
    plan.parse_retries = 0;
    plan.did_trim_messages = false;
    plan.did_retry_on_parse = false;
    plan.did_run_hook = false;
    plan.history_budget_chars_flush = flush_budget_chars;
    plan.memory_flush_threshold_ratio_used = ratio;

    if (req->hooks && req->hooks->on_budget) {
        context_hook_result_t hr = {0};
        mimi_err_t he = req->hooks->on_budget(req->hooks->user_ctx, &out->budget, &hr);
        plan.did_run_hook = true;
        if (he != MIMI_OK) {
            MIMI_LOGW(TAG, "context hook on_budget failed: %s", mimi_err_to_name(he));
        }
    }

    int mw = plan.memory_window_initial;
    const int max_retries = 6;

    for (int attempt = 0; attempt < max_retries; attempt++) {
        plan.parse_retries = attempt;

        if (req->hooks && req->hooks->pre_history_load) {
            context_hook_result_t hr = {0};
            int mw_inout = mw;
            mimi_err_t he = req->hooks->pre_history_load(req->hooks->user_ctx,
                                                         req->channel, req->chat_id,
                                                         req->base_memory_window,
                                                         &mw_inout,
                                                         &hr);
            plan.did_run_hook = true;
            if (he != MIMI_OK) {
                MIMI_LOGW(TAG, "context hook pre_history_load failed: %s", mimi_err_to_name(he));
            }
            if (mw_inout > 0) mw = mw_inout;
        }

        mimi_err_t err = session_get_history_json(req->channel, req->chat_id,
                                                   req->history_json_buf, req->history_json_buf_size,
                                                   mw);
        if (err != MIMI_OK) {
            strncpy(req->history_json_buf, "[]", req->history_json_buf_size - 1);
            req->history_json_buf[req->history_json_buf_size - 1] = '\0';
        }

        cJSON *messages = cJSON_Parse(req->history_json_buf);
        if (!messages || !cJSON_IsArray(messages)) {
            if (messages) cJSON_Delete(messages);
            plan.did_retry_on_parse = true;

            if (req->hooks && req->hooks->on_parse_error) {
                context_hook_result_t hr = {0};
                mimi_err_t he = req->hooks->on_parse_error(req->hooks->user_ctx,
                                                       req->channel, req->chat_id,
                                                       attempt, mw,
                                                       req->history_json_buf,
                                                       &hr);
                plan.did_run_hook = true;
                if (he != MIMI_OK) {
                    MIMI_LOGW(TAG, "context hook on_parse_error failed: %s", mimi_err_to_name(he));
                }
            }

            if (mw <= 1) break;
            mw = mw / 2;
            if (mw < 1) mw = 1;
            continue;
        }

        if (req->hooks && req->hooks->post_history_parsed) {
            context_hook_result_t hr = {0};
            mimi_err_t he = req->hooks->post_history_parsed(req->hooks->user_ctx,
                                                        req->channel, req->chat_id,
                                                        req->system_prompt_buf, req->system_prompt_buf_size,
                                                        messages,
                                                        &hr);
            plan.did_run_hook = true;
            if (he != MIMI_OK) {
                MIMI_LOGW(TAG, "context hook post_history_parsed failed: %s", mimi_err_to_name(he));
            }
        }

        if (req->user_content && req->user_content[0]) {
            (void)session_append(req->channel, req->chat_id, "user", req->user_content);
        }

        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", req->user_content ? req->user_content : "");
        cJSON_AddItemToArray(messages, user_msg);

        if (req->hooks && req->hooks->post_user_appended) {
            context_hook_result_t hr = {0};
            mimi_err_t he = req->hooks->post_user_appended(req->hooks->user_ctx,
                                                       req->channel, req->chat_id,
                                                       req->system_prompt_buf, req->system_prompt_buf_size,
                                                       messages,
                                                       &hr);
            plan.did_run_hook = true;
            if (he != MIMI_OK) {
                MIMI_LOGW(TAG, "context hook post_user_appended failed: %s", mimi_err_to_name(he));
            }
        }

        if (req->hooks && req->hooks->pre_trim) {
            context_hook_result_t hr = {0};
            mimi_err_t he = req->hooks->pre_trim(req->hooks->user_ctx,
                                            req->channel, req->chat_id,
                                            req->system_prompt_buf, req->system_prompt_buf_size,
                                            messages,
                                            &hr);
            plan.did_run_hook = true;
            if (he != MIMI_OK) {
                MIMI_LOGW(TAG, "context hook pre_trim failed: %s", mimi_err_to_name(he));
            }
        }

        bool did_trim = false;
        cJSON *trimmed_for_compact = NULL;
        trim_messages_to_budget(messages, flush_budget_chars, &did_trim, &trimmed_for_compact);

        out->trimmed_messages_for_compact = trimmed_for_compact;

        if (req->hooks && req->hooks->post_trim) {
            context_hook_result_t hr = {0};
            mimi_err_t he = req->hooks->post_trim(req->hooks->user_ctx,
                                              req->channel, req->chat_id,
                                              req->system_prompt_buf, req->system_prompt_buf_size,
                                              messages,
                                              &hr);
            plan.did_run_hook = true;
            if (he != MIMI_OK) {
                MIMI_LOGW(TAG, "context hook post_trim failed: %s", mimi_err_to_name(he));
            }
        }

        plan.memory_window_used = mw;
        plan.did_trim_messages = did_trim;
        out->messages = messages;
        out->plan = plan;
        return MIMI_OK;
    }

    cJSON *messages = cJSON_CreateArray();
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", req->user_content ? req->user_content : "");
    cJSON_AddItemToArray(messages, user_msg);

    if (req->user_content && req->user_content[0]) {
        (void)session_append(req->channel, req->chat_id, "user", req->user_content);
    }

    plan.memory_window_used = 1;
    plan.did_trim_messages = false;
    plan.history_budget_chars_flush = flush_budget_chars;
    plan.memory_flush_threshold_ratio_used = ratio;
    out->messages = messages;
    out->trimmed_messages_for_compact = NULL;
    out->plan = plan;

    MIMI_LOGW(TAG, "history assembly fallback: retries=%d", plan.parse_retries);
    return MIMI_OK;
}

