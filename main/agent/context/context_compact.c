#include "context_compact.h"

#include "log.h"
#include "llm/llm_trace.h"

#include <string.h>
#include <stdio.h>

#define TRACE_PREVIEW_MAX_BYTES 2048
#define TRACE_PREVIEW_MSGS_MAX 8
#define TRACE_PREVIEW_CONTENT_MAX_CHARS 220

mimi_err_t context_compact_insert_summary_message(cJSON *messages, cJSON *summary_message)
{
    if (!messages || !cJSON_IsArray(messages) || !summary_message || !cJSON_IsObject(summary_message)) {
        return MIMI_ERR_INVALID_ARG;
    }

    /* Insert at the beginning so summary covers removed oldest history. */
    if (cJSON_GetArraySize(messages) <= 0) {
        cJSON_AddItemToArray(messages, summary_message);
        return MIMI_OK;
    }

    cJSON_InsertItemInArray(messages, 0, summary_message);
    return MIMI_OK;
}

/* Compact/summary failure fallback merge helper. */
mimi_err_t context_compact_merge_compact_source_to_messages(cJSON **messages,
                                                             cJSON *compact_source_messages)
{
    if (!messages || !*messages || !cJSON_IsArray(*messages)) {
        return MIMI_ERR_INVALID_ARG;
    }
    if (!compact_source_messages || !cJSON_IsArray(compact_source_messages)) {
        return MIMI_ERR_INVALID_ARG;
    }

    cJSON *merged = cJSON_CreateArray();
    if (!merged) {
        return MIMI_ERR_NO_MEM;
    }

    /* Prepend compact_source_messages items in order. */
    while (cJSON_GetArraySize(compact_source_messages) > 0) {
        cJSON *it = cJSON_DetachItemFromArray(compact_source_messages, 0);
        if (!it) break;
        cJSON_AddItemToArray(merged, it);
    }

    /* Then append the newest main messages items in order. */
    while (cJSON_GetArraySize(*messages) > 0) {
        cJSON *it = cJSON_DetachItemFromArray(*messages, 0);
        if (!it) break;
        cJSON_AddItemToArray(merged, it);
    }

    cJSON_Delete(*messages);
    *messages = merged;
    return MIMI_OK;
}

#if MIMI_TRACE_CONTEXT_COMPACT_DETAILS
static size_t message_content_len(const cJSON *msg)
{
    if (!msg || !cJSON_IsObject((cJSON *)msg)) return 0;
    const cJSON *content = cJSON_GetObjectItem((cJSON *)msg, "content");
    if (!content || !cJSON_IsString((cJSON *)content) || !content->valuestring) return 0;
    return strlen(content->valuestring);
}

static size_t messages_total_content_len(const cJSON *messages)
{
    if (!messages || !cJSON_IsArray((cJSON *)messages)) return 0;
    int n = cJSON_GetArraySize((cJSON *)messages);
    size_t sum = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem((cJSON *)messages, i);
        sum += message_content_len(it);
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

static void build_messages_preview(const cJSON *messages,
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

void context_compact_trace_llm_input_meta(const char *trace_id, const cJSON *compact_source_messages)
{
    if (!trace_id || !trace_id[0]) return;
    if (!compact_source_messages || !cJSON_IsArray((cJSON *)compact_source_messages)) return;

    int n = cJSON_GetArraySize((cJSON *)compact_source_messages);
    if (n <= 0) return;

    size_t compact_chars = messages_total_content_len(compact_source_messages);
    char compact_preview[TRACE_PREVIEW_MAX_BYTES + 1];
    build_messages_preview(compact_source_messages,
                           compact_preview, sizeof(compact_preview),
                           TRACE_PREVIEW_MSGS_MAX, TRACE_PREVIEW_CONTENT_MAX_CHARS);

    char nbuf[32], charsbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%d", n);
    snprintf(charsbuf, sizeof(charsbuf), "%zu", compact_chars);

    llm_trace_event_kv(trace_id, "context_compact_llm_input_meta",
                       "compact_count", nbuf,
                       "compact_chars", charsbuf,
                       "preview", compact_preview,
                       NULL, NULL);
}

void context_compact_trace_failed_debug(const char *trace_id,
                                         const cJSON *main_messages,
                                         const cJSON *compact_source_messages)
{
    if (!trace_id || !trace_id[0]) return;
    if (!compact_source_messages || !cJSON_IsArray((cJSON *)compact_source_messages)) return;
    if (!main_messages || !cJSON_IsArray((cJSON *)main_messages)) return;

    int compact_n = cJSON_GetArraySize((cJSON *)compact_source_messages);
    size_t compact_chars = messages_total_content_len(compact_source_messages);
    char compact_preview[TRACE_PREVIEW_MAX_BYTES + 1];
    build_messages_preview(compact_source_messages,
                           compact_preview, sizeof(compact_preview),
                           TRACE_PREVIEW_MSGS_MAX, TRACE_PREVIEW_CONTENT_MAX_CHARS);

    int main_n = cJSON_GetArraySize((cJSON *)main_messages);

    char main_n_buf[32], compact_n_buf[32], compact_chars_buf[32];
    snprintf(compact_n_buf, sizeof(compact_n_buf), "%d", compact_n);
    snprintf(main_n_buf, sizeof(main_n_buf), "%d", main_n);
    snprintf(compact_chars_buf, sizeof(compact_chars_buf), "%zu", compact_chars);

    llm_trace_event_kv(trace_id, "context_compact_failed_debug",
                       "compact_count", compact_n_buf,
                       "main_count", main_n_buf,
                       "compact_chars", compact_chars_buf,
                       "preview", compact_preview);
}

void context_compact_trace_summary_output(const char *trace_id, const char *summary_text)
{
    if (!trace_id || !trace_id[0]) return;
    if (!summary_text) summary_text = "";

    size_t slen = summary_text ? strlen(summary_text) : 0;
    char summary_preview[TRACE_PREVIEW_MAX_BYTES + 1];
    normalize_for_preview(summary_preview, sizeof(summary_preview), summary_text, TRACE_PREVIEW_CONTENT_MAX_CHARS);
    char slen_buf[32];
    snprintf(slen_buf, sizeof(slen_buf), "%zu", slen);

    llm_trace_event_kv(trace_id, "context_compact_summary_output",
                       "summary_len", slen_buf,
                       "summary_preview", summary_preview,
                       NULL, NULL, NULL, NULL);
}

void context_compact_trace_messages_after(const char *trace_id, const cJSON *messages)
{
    if (!trace_id || !trace_id[0]) return;
    if (!messages || !cJSON_IsArray((cJSON *)messages)) return;

    int n_after = cJSON_GetArraySize((cJSON *)messages);

    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%d", n_after);

    char first_role[64] = {0};
    char first_preview[TRACE_PREVIEW_MAX_BYTES + 1];
    first_preview[0] = '\0';

    if (n_after > 0) {
        cJSON *first = cJSON_GetArrayItem((cJSON *)messages, 0);
        const cJSON *role = first ? cJSON_GetObjectItem(first, "role") : NULL;
        const cJSON *content = first ? cJSON_GetObjectItem(first, "content") : NULL;
        const char *role_s =
            (role && cJSON_IsString((cJSON *)role) && role->valuestring) ? role->valuestring : "?";
        normalize_for_preview(first_role, sizeof(first_role), role_s, 50);
        const char *content_s =
            (content && cJSON_IsString((cJSON *)content) && content->valuestring) ? content->valuestring : "";
        normalize_for_preview(first_preview, sizeof(first_preview), content_s, TRACE_PREVIEW_CONTENT_MAX_CHARS);
    }

    llm_trace_event_kv(trace_id, "context_compact_messages_after",
                       "messages_count", nbuf,
                       "first_role", first_role,
                       "first_preview", first_preview,
                       NULL, NULL);
}

void context_compact_trace_messages_after_failure(const char *trace_id, const cJSON *messages)
{
    if (!trace_id || !trace_id[0]) return;
    if (!messages || !cJSON_IsArray((cJSON *)messages)) return;

    int n_after = cJSON_GetArraySize((cJSON *)messages);
    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%d", n_after);

    char first_role[64] = {0};
    char first_preview[TRACE_PREVIEW_MAX_BYTES + 1];
    first_preview[0] = '\0';

    if (n_after > 0) {
        cJSON *first = cJSON_GetArrayItem((cJSON *)messages, 0);
        const cJSON *role = first ? cJSON_GetObjectItem(first, "role") : NULL;
        const cJSON *content = first ? cJSON_GetObjectItem(first, "content") : NULL;
        const char *role_s =
            (role && cJSON_IsString((cJSON *)role) && role->valuestring) ? role->valuestring : "?";
        normalize_for_preview(first_role, sizeof(first_role), role_s, 50);
        const char *content_s =
            (content && cJSON_IsString((cJSON *)content) && content->valuestring) ? content->valuestring : "";
        normalize_for_preview(first_preview, sizeof(first_preview), content_s, TRACE_PREVIEW_CONTENT_MAX_CHARS);
    }

    llm_trace_event_kv(trace_id, "context_compact_messages_after_failure",
                       "messages_count", nbuf,
                       "first_role", first_role,
                       "first_preview", first_preview,
                       NULL, NULL);
}
#else
void context_compact_trace_llm_input_meta(const char *trace_id, const cJSON *compact_source_messages) {(void)trace_id; (void)compact_source_messages;}
void context_compact_trace_failed_debug(const char *trace_id, const cJSON *main_messages, const cJSON *compact_source_messages)
{(void)trace_id; (void)main_messages; (void)compact_source_messages;}
void context_compact_trace_summary_output(const char *trace_id, const char *summary_text){(void)trace_id; (void)summary_text;}
void context_compact_trace_messages_after(const char *trace_id, const cJSON *messages){(void)trace_id; (void)messages;}
void context_compact_trace_messages_after_failure(const char *trace_id, const cJSON *messages){(void)trace_id; (void)messages;}
#endif

