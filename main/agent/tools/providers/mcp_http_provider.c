#include "tools/providers/mcp_provider_internal.h"

#include "http/http.h"
#include "log.h"
#include "os/os.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "mcp_http_provider";
static const char *MCP_PROTOCOL_VERSION = "2025-11-25";
static const uint32_t MCP_HTTP_TIMEOUT_MS = 3000;
static const uint32_t MCP_SSE_WAIT_TIMEOUT_MS = 9000;
/* Match test/integration/webparse_simple.py: httpx.Timeout(30.0, read=60.0) */
static const uint32_t MCP_HTTP_SSE_CONNECT_MS = 30000;
static const uint32_t MCP_HTTP_SSE_READ_IDLE_MS = 60000;
static const int MCP_HTTP_429_MAX_RETRIES = 4;
static const uint32_t MCP_HTTP_429_BASE_DELAY_MS = 1200;

static const char *MCP_CAPTURE_HEADERS[] = {
    "MCP-Session-Id",
};
static const size_t MCP_CAPTURE_HEADERS_COUNT = 1;

static const char *mcp_session_id_from_resp(const mimi_http_response_t *resp)
{
    return mimi_http_get_captured_header_value(resp, "MCP-Session-Id");
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static bool jsonrpc_id_matches(cJSON *mid, const char *id_str)
{
    if (!mid || !id_str) return false;
    if (cJSON_IsString(mid)) return strcmp(mid->valuestring, id_str) == 0;
    if (cJSON_IsNumber(mid)) {
        unsigned long long expected = (unsigned long long)strtoull(id_str, NULL, 10);
        unsigned long long got = (unsigned long long)mid->valuedouble;
        return got == expected;
    }
    return false;
}

static char *process_jsonrpc_message(mcp_server_t *s, const char *json_text, const char *expect_id,
                                     mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request)
{
    if (!json_text || !json_text[0]) return NULL;
    cJSON *msg = cJSON_Parse(json_text);
    if (!msg || !cJSON_IsObject(msg)) {
        cJSON_Delete(msg);
        return NULL;
    }
    cJSON *mid = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (expect_id && mid && jsonrpc_id_matches(mid, expect_id)) {
        char *ret = strdup(json_text);
        cJSON_Delete(msg);
        return ret;
    }
    cJSON *mmethod = cJSON_GetObjectItemCaseSensitive(msg, "method");
    if (mmethod && cJSON_IsString(mmethod)) {
        if (mid) {
            if (on_request) on_request(s, msg);
        } else {
            if (on_notification) on_notification(s, msg);
        }
    }
    cJSON_Delete(msg);
    return NULL;
}

static char *parse_sse_for_response(mcp_server_t *s, const char *body, const char *expect_id,
                                    mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request)
{
    if (!s || !body) return NULL;
    size_t cap = 65536;
    char *data_buf = (char *)malloc(cap);
    if (!data_buf) return NULL;
    size_t data_len = 0;
    char event_id[128] = {0};
    const char *p = body;

    while (p && *p) {
        const char *line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        const char *line = p;
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;
        bool dispatch = (line_len == 0);

        if (!dispatch) {
            if (line_len >= 3 && strncmp(line, "id:", 3) == 0) {
                const char *v = line + 3;
                while (*v == ' ') v++;
                size_t n = line_len - (size_t)(v - line);
                if (n >= sizeof(event_id)) n = sizeof(event_id) - 1;
                memcpy(event_id, v, n);
                event_id[n] = '\0';
            } else if (line_len >= 6 && strncmp(line, "retry:", 6) == 0) {
                const char *v = line + 6;
                while (*v == ' ') v++;
                int r = atoi(v);
                if (r > 0 && r < 120000) s->sse_retry_ms = r;
            } else if (line_len >= 5 && strncmp(line, "data:", 5) == 0) {
                const char *v = line + 5;
                while (*v == ' ') v++;
                size_t n = line_len - (size_t)(v - line);
                if (n > 0) {
                    if (data_len + n + 2 > cap) {
                        size_t new_cap = cap;
                        while (new_cap < data_len + n + 2) {
                            new_cap *= 2;
                        }
                        char *grown = (char *)realloc(data_buf, new_cap);
                        if (!grown) {
                            free(data_buf);
                            return NULL;
                        }
                        data_buf = grown;
                        cap = new_cap;
                    }
                    memcpy(data_buf + data_len, v, n);
                    data_len += n;
                    data_buf[data_len++] = '\n';
                }
            }
        }

        if (dispatch) {
            if (event_id[0]) {
                strncpy(s->last_event_id, event_id, sizeof(s->last_event_id) - 1);
                s->last_event_id[sizeof(s->last_event_id) - 1] = '\0';
            }
            if (data_len > 0) {
                if (data_buf[data_len - 1] == '\n') data_len--;
                data_buf[data_len] = '\0';
                char *ret = process_jsonrpc_message(s, data_buf, expect_id, on_notification, on_request);
                if (ret) {
                    free(data_buf);
                    return ret;
                }
            }
            data_len = 0;
            event_id[0] = '\0';
        }
        p = line_end ? (line_end + 1) : NULL;
    }
    free(data_buf);
    return NULL;
}

static void mcp_sse_build_poll_headers(mcp_server_t *s, char *out, size_t out_sz);
static const char *effective_url(const char *url, char *buf, size_t buf_sz);
static bool url_join_origin_path(const char *base_url, const char *path, char *out, size_t out_sz);

typedef struct {
    mcp_server_t *s;
    mimi_mutex_t *mu;
    mimi_cond_t *cv;
    mimi_http_stream_handle_t *stream_handle;
    bool opened;
    bool closed;
    mimi_err_t close_err;
    char wait_id[64];
    char *matched_resp;
    mcp_server_msg_cb_t on_notification;
    mcp_server_msg_cb_t on_request;
} mcp_legacy_sse_pump_t;

static mcp_legacy_sse_pump_t g_legacy_sse_pumps[8];

static void mcp_legacy_update_endpoint_from_event(mcp_server_t *s, const char *data)
{
    if (!s || !data || !data[0]) return;
    char msg_url[512] = {0};
    if (strncmp(data, "http://", 7) == 0 || strncmp(data, "https://", 8) == 0) {
        snprintf(msg_url, sizeof(msg_url), "%s", data);
    } else {
        if (!url_join_origin_path(s->url, data, msg_url, sizeof(msg_url))) return;
    }
    if (msg_url[0]) {
        strncpy(s->sse_message_url, msg_url, sizeof(s->sse_message_url) - 1);
        s->sse_message_url[sizeof(s->sse_message_url) - 1] = '\0';
    }

    const char *sid_key = strstr(msg_url, "sessionId=");
    if (!sid_key) sid_key = strstr(msg_url, "session_id=");
    if (sid_key) {
        const char *sid = strchr(sid_key, '=');
        if (sid) sid++;
        if (sid && *sid) {
            const char *end = sid;
            while (*end && *end != '&' && *end != ' ' && *end != '\r' && *end != '\n') end++;
            size_t n = (size_t)(end - sid);
            if (n > 0) {
                if (n >= sizeof(s->session_id)) n = sizeof(s->session_id) - 1;
                memcpy(s->session_id, sid, n);
                s->session_id[n] = '\0';
            }
        }
    }
}

static mcp_legacy_sse_pump_t *mcp_get_legacy_sse_pump(mcp_server_t *s, bool create_if_missing)
{
    if (!s) return NULL;
    mcp_legacy_sse_pump_t *free_slot = NULL;
    for (size_t i = 0; i < sizeof(g_legacy_sse_pumps) / sizeof(g_legacy_sse_pumps[0]); i++) {
        mcp_legacy_sse_pump_t *p = &g_legacy_sse_pumps[i];
        if (p->s == s) return p;
        if (!p->s && !free_slot) free_slot = p;
    }
    if (!create_if_missing || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->s = s;
    if (mimi_mutex_create(&free_slot->mu) != MIMI_OK) {
        memset(free_slot, 0, sizeof(*free_slot));
        return NULL;
    }
    if (mimi_cond_create(&free_slot->cv) != MIMI_OK) {
        mimi_mutex_destroy(free_slot->mu);
        memset(free_slot, 0, sizeof(*free_slot));
        return NULL;
    }
    return free_slot;
}

static void mcp_legacy_sse_pump_on_open(const mimi_http_response_t *meta, void *user_data)
{
    mcp_legacy_sse_pump_t *p = (mcp_legacy_sse_pump_t *)user_data;
    if (!p || !p->mu || !p->cv) return;
    MIMI_LOGI(TAG, "SSE pump open: server=%s status=%d content_type=%s",
              p->s ? p->s->name : "(null)",
              meta ? meta->status : 0,
              (meta && meta->content_type) ? meta->content_type : "(null)");
    (void)mimi_mutex_lock(p->mu);
    p->opened = true;
    (void)mimi_cond_signal(p->cv);
    (void)mimi_mutex_unlock(p->mu);
}

static void mcp_legacy_sse_pump_on_event_ex(const char *event_name,
                                            const char *data,
                                            const char *event_id,
                                            int retry_ms,
                                            void *user_data)
{
    mcp_legacy_sse_pump_t *p = (mcp_legacy_sse_pump_t *)user_data;
    if (!p || !p->s || !p->mu || !p->cv || !data) return;
    if (event_id && event_id[0]) {
        strncpy(p->s->last_event_id, event_id, sizeof(p->s->last_event_id) - 1);
        p->s->last_event_id[sizeof(p->s->last_event_id) - 1] = '\0';
    }
    if (retry_ms > 0 && retry_ms < 120000) p->s->sse_retry_ms = retry_ms;
    MIMI_LOGI(TAG, "SSE pump event: server=%s event=%s id=%s retry=%d data=%.*s",
              p->s->name,
              (event_name && event_name[0]) ? event_name : "(none)",
              (event_id && event_id[0]) ? event_id : "(none)",
              retry_ms,
              (int)(strlen(data) > 160 ? 160 : strlen(data)), data);

    /* DashScope legacy behavior: endpoint updates can arrive as SSE events. */
    if (event_name && strcmp(event_name, "endpoint") == 0) {
        mcp_legacy_update_endpoint_from_event(p->s, data);
    }

    (void)mimi_mutex_lock(p->mu);
    const char *expect_id = p->wait_id[0] ? p->wait_id : NULL;
    char *matched = process_jsonrpc_message(p->s, data, expect_id, p->on_notification, p->on_request);
    if (matched) {
        if (p->matched_resp) free(p->matched_resp);
        p->matched_resp = matched;
        (void)mimi_cond_signal(p->cv);
    }
    (void)mimi_mutex_unlock(p->mu);
}

static void mcp_legacy_sse_pump_on_close(mimi_err_t err, void *user_data)
{
    mcp_legacy_sse_pump_t *p = (mcp_legacy_sse_pump_t *)user_data;
    if (!p || !p->mu || !p->cv) return;
    MIMI_LOGI(TAG, "SSE pump close: server=%s err=%d", p->s ? p->s->name : "(null)", err);
    (void)mimi_mutex_lock(p->mu);
    p->closed = true;
    p->close_err = err;
    p->stream_handle = NULL;
    (void)mimi_cond_signal(p->cv);
    (void)mimi_mutex_unlock(p->mu);
}

static bool mcp_legacy_sse_pump_ensure(mcp_server_t *s,
                                       mcp_server_msg_cb_t on_notification,
                                       mcp_server_msg_cb_t on_request)
{
    if (!s || !s->url[0]) return false;
    mcp_legacy_sse_pump_t *p = mcp_get_legacy_sse_pump(s, true);
    if (!p) return false;
    (void)mimi_mutex_lock(p->mu);
    p->on_notification = on_notification;
    p->on_request = on_request;
    bool need_start = (p->stream_handle == NULL || p->closed);
    if (need_start) {
        p->opened = false;
        p->closed = false;
        p->close_err = MIMI_OK;
        p->wait_id[0] = '\0';
        if (p->matched_resp) {
            free(p->matched_resp);
            p->matched_resp = NULL;
        }
    }
    (void)mimi_mutex_unlock(p->mu);

    if (!need_start) return true;

    char sse_url_buf[768];
    const char *sse_url = effective_url(s->url, sse_url_buf, sizeof(sse_url_buf));
    char poll_headers[1024];
    mcp_sse_build_poll_headers(s, poll_headers, sizeof(poll_headers));

    static const mimi_http_stream_callbacks_t cb = {
        .on_open = mcp_legacy_sse_pump_on_open,
        .on_sse_event = NULL,
        .on_sse_event_ex = mcp_legacy_sse_pump_on_event_ex,
        .on_data = NULL,
        .on_close = mcp_legacy_sse_pump_on_close,
    };

    mimi_http_stream_handle_t *handle = NULL;
    mimi_http_request_t req = {
        .method = "GET",
        .url = sse_url,
        .headers = poll_headers,
        .timeout_ms = MCP_HTTP_SSE_CONNECT_MS,
        .stream_read_idle_ms = MCP_HTTP_SSE_READ_IDLE_MS,
        .mode = MIMI_HTTP_CALL_STREAM,
        .stream_callbacks = &cb,
        .out_stream_handle = &handle,
        .capture_response_headers = MCP_CAPTURE_HEADERS,
        .capture_response_headers_count = MCP_CAPTURE_HEADERS_COUNT,
    };
    mimi_err_t err = mimi_http_exec_async(&req, NULL, NULL, p);
    if (err != MIMI_OK || !handle) return false;

    (void)mimi_mutex_lock(p->mu);
    p->stream_handle = handle;
    long long deadline = now_ms() + 1500;
    while (!p->opened && !p->closed) {
        long long rem = deadline - now_ms();
        if (rem <= 0) break;
        (void)mimi_cond_wait(p->cv, p->mu, (uint32_t)rem);
    }
    bool ok = p->opened && !p->closed;
    (void)mimi_mutex_unlock(p->mu);
    return ok;
}

/* So SSE can match JSON-RPC results that arrive while the synchronous POST is still in flight. */
static void mcp_legacy_sse_pump_arm(mcp_server_t *s, const char *id_str)
{
    if (!s || !id_str || !id_str[0]) return;
    mcp_legacy_sse_pump_t *p = mcp_get_legacy_sse_pump(s, false);
    if (!p || !p->mu) return;
    (void)mimi_mutex_lock(p->mu);
    snprintf(p->wait_id, sizeof(p->wait_id), "%s", id_str);
    if (p->matched_resp) {
        free(p->matched_resp);
        p->matched_resp = NULL;
    }
    (void)mimi_mutex_unlock(p->mu);
}

static void mcp_legacy_sse_pump_disarm(mcp_server_t *s)
{
    mcp_legacy_sse_pump_t *p = mcp_get_legacy_sse_pump(s, false);
    if (!p || !p->mu) return;
    (void)mimi_mutex_lock(p->mu);
    p->wait_id[0] = '\0';
    if (p->matched_resp) {
        free(p->matched_resp);
        p->matched_resp = NULL;
    }
    (void)mimi_mutex_unlock(p->mu);
}

static char *mcp_legacy_sse_pump_wait_response(mcp_server_t *s, const char *id_str, uint32_t timeout_ms)
{
    if (!s || !id_str) return NULL;
    mcp_legacy_sse_pump_t *p = mcp_get_legacy_sse_pump(s, false);
    if (!p || !p->mu || !p->cv) return NULL;
    long long deadline = now_ms() + (long long)timeout_ms;
    for (;;) {
        (void)mimi_mutex_lock(p->mu);
        snprintf(p->wait_id, sizeof(p->wait_id), "%s", id_str);
        if (p->matched_resp) {
            char *ret = p->matched_resp;
            p->matched_resp = NULL;
            p->wait_id[0] = '\0';
            (void)mimi_mutex_unlock(p->mu);
            return ret;
        }
        while (!p->matched_resp && !p->closed) {
            long long rem = deadline - now_ms();
            if (rem <= 0) break;
            (void)mimi_cond_wait(p->cv, p->mu, (uint32_t)rem);
        }
        if (p->matched_resp) {
            char *ret = p->matched_resp;
            p->matched_resp = NULL;
            p->wait_id[0] = '\0';
            (void)mimi_mutex_unlock(p->mu);
            return ret;
        }
        bool was_closed = p->closed;
        p->wait_id[0] = '\0';
        (void)mimi_mutex_unlock(p->mu);

        long long rem = deadline - now_ms();
        if (rem <= 0) return NULL;
        if (!was_closed) return NULL;

        /* Stream closed before response; reconnect and keep waiting until timeout. */
        (void)mcp_legacy_sse_pump_ensure(s, p->on_notification, p->on_request);
    }
}

typedef struct {
    mcp_server_t *s;
    char *expect_id; /* owned copy */
    mcp_server_msg_cb_t on_notification;
    mcp_server_msg_cb_t on_request;
    mimi_mutex_t *mu;
    mimi_cond_t *cv;
    bool done;
    bool stream_closed;
    bool stream_opened;
    bool should_free_on_close;
    mimi_err_t close_err;
    char *ret; /* owned result; freed by waiter if not returned */
    mimi_http_stream_handle_t *stream_handle;
} mcp_sse_stream_wait_ctx_t;

static void mcp_sse_on_open(const mimi_http_response_t *meta, void *user_data)
{
    mcp_sse_stream_wait_ctx_t *ctx = (mcp_sse_stream_wait_ctx_t *)user_data;
    if (!ctx || !ctx->mu || !ctx->cv) return;
    MIMI_LOGI(TAG, "SSE wait open: server=%s status=%d content_type=%s",
              ctx->s ? ctx->s->name : "(null)",
              meta ? meta->status : 0,
              (meta && meta->content_type) ? meta->content_type : "(null)");
    (void)mimi_mutex_lock(ctx->mu);
    ctx->stream_opened = true;
    (void)mimi_cond_signal(ctx->cv);
    (void)mimi_mutex_unlock(ctx->mu);
}

static void mcp_sse_on_event_ex(const char *event_name,
                                const char *data,
                                const char *event_id,
                                int retry_ms,
                                void *user_data)
{
    (void)event_name;
    mcp_sse_stream_wait_ctx_t *ctx = (mcp_sse_stream_wait_ctx_t *)user_data;
    if (!ctx || !ctx->s || !data) return;

    if (event_id && event_id[0]) {
        strncpy(ctx->s->last_event_id, event_id, sizeof(ctx->s->last_event_id) - 1);
        ctx->s->last_event_id[sizeof(ctx->s->last_event_id) - 1] = '\0';
    }
    if (retry_ms > 0 && retry_ms < 120000) {
        ctx->s->sse_retry_ms = retry_ms;
    }
    if (data && data[0]) {
        size_t n = strlen(data);
        if (n > 160) n = 160;
        MIMI_LOGI(TAG, "SSE wait event: server=%s event=%s id=%s retry=%d data=%.*s",
                  ctx->s->name,
                  (event_name && event_name[0]) ? event_name : "(none)",
                  (event_id && event_id[0]) ? event_id : "(none)",
                  retry_ms,
                  (int)n, data);
    } else {
        MIMI_LOGI(TAG, "SSE wait event: server=%s event=%s id=%s retry=%d data=(empty)",
                  ctx->s->name,
                  (event_name && event_name[0]) ? event_name : "(none)",
                  (event_id && event_id[0]) ? event_id : "(none)",
                  retry_ms);
    }

    char *matched = process_jsonrpc_message(ctx->s, data, ctx->expect_id, ctx->on_notification, ctx->on_request);
    if (matched) {
        (void)mimi_mutex_lock(ctx->mu);
        if (!ctx->ret) ctx->ret = matched;
        else free(matched);
        ctx->done = true;
        (void)mimi_cond_signal(ctx->cv);
        (void)mimi_mutex_unlock(ctx->mu);
    }
}

static void mcp_sse_on_close(mimi_err_t err, void *user_data)
{
    mcp_sse_stream_wait_ctx_t *ctx = (mcp_sse_stream_wait_ctx_t *)user_data;
    if (!ctx) return;
    MIMI_LOGI(TAG, "SSE wait close: server=%s err=%d", ctx->s ? ctx->s->name : "(null)", err);
    if (ctx->mu && ctx->cv) {
        (void)mimi_mutex_lock(ctx->mu);
        ctx->close_err = err;
        ctx->stream_closed = true;
        ctx->done = true;
        (void)mimi_cond_signal(ctx->cv);
        (void)mimi_mutex_unlock(ctx->mu);
    } else {
        ctx->close_err = err;
        ctx->stream_closed = true;
        ctx->done = true;
    }

    if (ctx->should_free_on_close) {
        if (ctx->ret) {
            free(ctx->ret);
            ctx->ret = NULL;
        }
        if (ctx->expect_id) {
            free(ctx->expect_id);
            ctx->expect_id = NULL;
        }
        if (ctx->cv) mimi_cond_destroy(ctx->cv);
        if (ctx->mu) mimi_mutex_destroy(ctx->mu);
        free(ctx);
    }
}

static mcp_sse_stream_wait_ctx_t *mcp_sse_stream_wait_start(mcp_server_t *s,
                                                            const char *sse_url,
                                                            const char *expect_id,
                                                            mcp_server_msg_cb_t on_notification,
                                                            mcp_server_msg_cb_t on_request)
{
    if (!s || !sse_url || !expect_id) return NULL;

    mcp_sse_stream_wait_ctx_t *ctx = (mcp_sse_stream_wait_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->s = s;
    ctx->expect_id = strdup(expect_id);
    ctx->on_notification = on_notification;
    ctx->on_request = on_request;
    ctx->done = false;
    ctx->stream_closed = false;
    ctx->stream_opened = false;
    ctx->should_free_on_close = false;
    ctx->close_err = MIMI_OK;
    ctx->ret = NULL;
    ctx->stream_handle = NULL;

    if (!ctx->expect_id) {
        free(ctx);
        return NULL;
    }

    ctx->mu = NULL;
    ctx->cv = NULL;
    if (mimi_mutex_create(&ctx->mu) != MIMI_OK || mimi_cond_create(&ctx->cv) != MIMI_OK) {
        if (ctx->cv) mimi_cond_destroy(ctx->cv);
        if (ctx->mu) mimi_mutex_destroy(ctx->mu);
        free(ctx->expect_id);
        free(ctx);
        return NULL;
    }

    char poll_headers[1024];
    mcp_sse_build_poll_headers(s, poll_headers, sizeof(poll_headers));

    static const mimi_http_stream_callbacks_t cb = {
        .on_open = mcp_sse_on_open,
        .on_sse_event = NULL,
        .on_sse_event_ex = mcp_sse_on_event_ex,
        .on_data = NULL,
        .on_close = mcp_sse_on_close,
    };

    mimi_http_stream_handle_t *handle = NULL;
    mimi_http_request_t req = {
        .method = "GET",
        .url = sse_url,
        .headers = poll_headers,
        .timeout_ms = MCP_HTTP_SSE_CONNECT_MS,
        .stream_read_idle_ms = MCP_HTTP_SSE_READ_IDLE_MS,
        .mode = MIMI_HTTP_CALL_STREAM,
        .stream_callbacks = &cb,
        .out_stream_handle = &handle,
        .capture_response_headers = MCP_CAPTURE_HEADERS,
        .capture_response_headers_count = MCP_CAPTURE_HEADERS_COUNT,
    };

    mimi_err_t err = mimi_http_exec_async(&req, NULL, NULL, ctx);
    if (err != MIMI_OK || !handle) {
        mimi_cond_destroy(ctx->cv);
        mimi_mutex_destroy(ctx->mu);
        free(ctx->expect_id);
        free(ctx);
        return NULL;
    }
    ctx->stream_handle = handle;

    /* Best-effort barrier: wait a short time for stream open callback so
     * subsequent POST does not race ahead of SSE subscription readiness. */
    long long open_deadline = now_ms() + 1500;
    (void)mimi_mutex_lock(ctx->mu);
    while (!ctx->stream_opened && !ctx->stream_closed) {
        long long remaining = open_deadline - now_ms();
        if (remaining <= 0) break;
        (void)mimi_cond_wait(ctx->cv, ctx->mu, (uint32_t)remaining);
    }
    (void)mimi_mutex_unlock(ctx->mu);

    return ctx;
}

static void mcp_sse_stream_wait_finish(mcp_sse_stream_wait_ctx_t *ctx, uint32_t timeout_ms_wait, char **inout_ret)
{
    if (!ctx || !inout_ret) return;
    long long start_wait = now_ms();

    (void)mimi_mutex_lock(ctx->mu);
    while (!ctx->done) {
        long long elapsed = now_ms() - start_wait;
        if ((uint32_t)elapsed >= timeout_ms_wait) break;
        (void)mimi_cond_wait(ctx->cv, ctx->mu, timeout_ms_wait - (uint32_t)elapsed);
    }
    (void)mimi_mutex_unlock(ctx->mu);

    if (ctx->stream_handle) {
        mimi_http_exec_async_cancel(ctx->stream_handle);
        mimi_http_stream_handle_release(ctx->stream_handle);
        ctx->stream_handle = NULL;
    }

    /* Wait until the stream is actually closed so callbacks won't run
     * after we destroy ctx->mu/ctx->cv/ctx itself. */
    bool can_free_now = false;
    (void)mimi_mutex_lock(ctx->mu);
    long long close_deadline = start_wait + (long long)timeout_ms_wait;
    while (!ctx->stream_closed) {
        long long remaining = close_deadline - now_ms();
        if (remaining <= 0) break;
        (void)mimi_cond_wait(ctx->cv, ctx->mu, (uint32_t)remaining);
    }
    can_free_now = ctx->stream_closed;

    if (!*inout_ret && ctx->ret) {
        *inout_ret = ctx->ret;
        ctx->ret = NULL;
    } else if (*inout_ret && ctx->ret) {
        free(ctx->ret);
        ctx->ret = NULL;
    }

    if (!can_free_now) {
        /* Late on_close might still arrive after we return.
         * Defer cleanup to the on_close callback to avoid UAF. */
        ctx->should_free_on_close = true;
        (void)mimi_mutex_unlock(ctx->mu);
        return;
    }
    (void)mimi_mutex_unlock(ctx->mu);

    /* Safe to destroy primitives now. */
    free(ctx->ret);
    free(ctx->expect_id);
    mimi_cond_destroy(ctx->cv);
    mimi_mutex_destroy(ctx->mu);
    free(ctx);
}

static void build_http_headers(mcp_server_t *s, bool sse_only, char *headers, size_t headers_sz)
{
    const char *pv = s->negotiated_protocol_version[0] ? s->negotiated_protocol_version : MCP_PROTOCOL_VERSION;
    const char *accept = sse_only ? "text/event-stream" : "application/json, text/event-stream";
    int off = 0;
    if (s->session_id[0]) {
        off = snprintf(headers, headers_sz,
                 "Content-Type: application/json\r\n"
                 "Accept: %s\r\n"
                 "MCP-Protocol-Version: %s\r\n"
                 "MCP-Session-Id: %s\r\n",
                 accept, pv, s->session_id);
    } else {
        off = snprintf(headers, headers_sz,
                 "Content-Type: application/json\r\n"
                 "Accept: %s\r\n"
                 "MCP-Protocol-Version: %s\r\n",
                 accept, pv);
    }

    MIMI_LOGD(TAG, "build_http_headers: extra_http_headers_len=%zu session_id=[%s]",
              strlen(s->extra_http_headers), s->session_id);
    if (off > 0 && s->extra_http_headers[0]) {
        size_t extra_len = strlen(s->extra_http_headers);
        if ((size_t)off + extra_len < headers_sz) {
            memcpy(headers + off, s->extra_http_headers, extra_len);
            headers[off + extra_len] = '\0';
        }
    }
}

static bool url_extract_origin(const char *url, char *out, size_t out_sz)
{
    if (!url || !out || out_sz == 0) return false;
    out[0] = '\0';
    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - url) : strlen(url);
    if (n == 0 || n >= out_sz) return false;
    memcpy(out, url, n);
    out[n] = '\0';
    return true;
}

static bool url_join_origin_path(const char *base_url, const char *path, char *out, size_t out_sz)
{
    if (!base_url || !path || !out || out_sz == 0) return false;
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        snprintf(out, out_sz, "%s", path);
        return true;
    }
    char origin[256];
    if (!url_extract_origin(base_url, origin, sizeof(origin))) return false;
    if (path[0] == '/') snprintf(out, out_sz, "%s%s", origin, path);
    else snprintf(out, out_sz, "%s/%s", origin, path);
    return out[0] != '\0';
}

static const char *effective_url(const char *url, char *buf, size_t buf_sz)
{
    if (!url || !buf || buf_sz == 0) return url;
    const char *http_prefix = "http://127.0.0.1";
    const char *https_prefix = "https://127.0.0.1";
    const char *replacement = NULL;
    size_t prefix_len = 0;
    if (strncmp(url, http_prefix, strlen(http_prefix)) == 0) {
        replacement = "http://localhost";
        prefix_len = strlen(http_prefix);
    } else if (strncmp(url, https_prefix, strlen(https_prefix)) == 0) {
        replacement = "https://localhost";
        prefix_len = strlen(https_prefix);
    }
    if (!replacement) return url;
    snprintf(buf, buf_sz, "%s%s", replacement, url + prefix_len);
    return buf;
}

static bool parse_sse_endpoint_message(const char *body, size_t body_len,
                                         char *out_message_url, size_t out_message_url_sz,
                                         char *out_session_id, size_t out_session_id_sz,
                                         const char *base_url_for_origin)
{
    if (!body || !out_message_url || !out_session_id) return false;
    out_message_url[0] = '\0';
    out_session_id[0] = '\0';

    const char *body_end = body + body_len;

    /* Expected legacy SSE event:
     *   event:endpoint
     *   data:/api/.../message?sessionId=...
     *
     * Some servers insert the HTTP chunk size as an extra line (e.g. "4d\\r\\n/path...").
     * We detect that and skip the chunk-size line if the next line looks like a URL path.
     */
    const char *event_line = strstr(body, "event:endpoint");
    if (!event_line) return false;

    const char *data_colon = strstr(event_line, "data:");
    if (!data_colon) return false;
    data_colon += strlen("data:");
    while (data_colon < body_end && (*data_colon == ' ' || *data_colon == '\t')) data_colon++;
    if (data_colon >= body_end) return false;

    const char *first_end = strpbrk(data_colon, "\r\n");
    if (!first_end || first_end > body_end) return false;
    size_t first_len = (size_t)(first_end - data_colon);

    const char *path_start = data_colon;
    size_t path_len = first_len;

    /* Some servers send an empty `data:` line and put the actual data on the next line(s),
     * sometimes preceded by an HTTP chunk-size line.
     * Example (DashScope observed):
     *   event:endpoint
     *   data:
     *   4d
     *   /api/.../message?sessionId=...
     */
    if (first_len == 0) {
        const char *after = first_end;
        while (after < body_end && (*after == '\r' || *after == '\n' || *after == ' ' || *after == '\t')) after++;
        if (after >= body_end) return false;
        const char *next_end = strpbrk(after, "\r\n");
        if (!next_end || next_end > body_end) return false;
        path_start = after;
        path_len = (size_t)(next_end - after);
    }

    /* Chunk-size heuristic: if first line is short hex and the next non-ws begins with '/', use that next line. */
    if (first_len > 0 && first_len <= 8) {
        char *hex_end = NULL;
        unsigned long maybe_chunk = strtoul(data_colon, &hex_end, 16);
        if (hex_end && hex_end != data_colon && maybe_chunk > 0 && maybe_chunk < 8192) {
            const char *after = first_end;
            while (after < body_end && (*after == '\r' || *after == '\n' || *after == ' ' || *after == '\t')) after++;
            if (after < body_end && *after == '/') {
                const char *next_end = strpbrk(after, "\r\n");
                if (next_end && next_end <= body_end) {
                    path_start = after;
                    path_len = (size_t)(next_end - after);
                }
            }
        }
    }

    /* If `data:` was empty, path_start now points to the first line after `data:`.
     * Apply the same chunk-size heuristic to that line. */
    if (first_len == 0 && path_len > 0 && path_len <= 8) {
        char tmp[16];
        size_t n = path_len >= sizeof(tmp) ? sizeof(tmp) - 1 : path_len;
        memcpy(tmp, path_start, n);
        tmp[n] = '\0';

        char *hex_end = NULL;
        unsigned long maybe_chunk = strtoul(tmp, &hex_end, 16);
        if (hex_end && hex_end != tmp && maybe_chunk > 0 && maybe_chunk < 8192) {
            const char *after = path_start + path_len;
            while (after < body_end && (*after == '\r' || *after == '\n' || *after == ' ' || *after == '\t')) after++;
            if (after < body_end && *after == '/') {
                const char *next_end = strpbrk(after, "\r\n");
                if (next_end && next_end <= body_end) {
                    path_start = after;
                    path_len = (size_t)(next_end - after);
                }
            }
        }
    }

    if (path_len == 0 || path_len >= 4096) return false;
    char path_buf[4096];
    memcpy(path_buf, path_start, path_len);
    path_buf[path_len] = '\0';

    /* Extract sessionId if present in query. */
    const char *sid_key = strstr(path_buf, "sessionId=");
    size_t sid_key_len = sid_key ? strlen("sessionId=") : 0;
    if (!sid_key) {
        sid_key = strstr(path_buf, "session_id=");
        sid_key_len = sid_key ? strlen("session_id=") : 0;
    }
    if (sid_key && sid_key_len > 0) {
        const char *sid = sid_key + sid_key_len;
        const char *end = sid;
        while (*end && *end != '&' && *end != ' ' && *end != '\r' && *end != '\n') end++;
        size_t sid_len = (size_t)(end - sid);
        if (sid_len > 0) {
            if (sid_len >= out_session_id_sz) sid_len = out_session_id_sz - 1;
            memcpy(out_session_id, sid, sid_len);
            out_session_id[sid_len] = '\0';
        }
    }

    if (!base_url_for_origin || !base_url_for_origin[0]) return false;
    if (!url_join_origin_path(base_url_for_origin, path_buf, out_message_url, out_message_url_sz)) return false;
    return out_message_url[0] != '\0';
}

typedef struct {
    char message_url[512];
    char session_id[192];
} mcp_legacy_endpoint_info_t;

static bool mcp_legacy_parse_endpoint_event(const mimi_http_response_t *gresp,
                                            const char *base_url_for_origin,
                                            mcp_legacy_endpoint_info_t *out)
{
    if (!gresp || !out || !base_url_for_origin) return false;
    memset(out, 0, sizeof(*out));
    if (gresp->status >= 400 || !gresp->body || gresp->body_len == 0) return false;
    return parse_sse_endpoint_message((const char *)gresp->body, gresp->body_len,
                                      out->message_url, sizeof(out->message_url),
                                      out->session_id, sizeof(out->session_id),
                                      base_url_for_origin);
}

static mimi_err_t mcp_legacy_fetch_endpoint_event(mcp_server_t *s,
                                                  const char *sse_url,
                                                  mimi_http_response_t *out_resp)
{
    if (!s || !sse_url || !out_resp) return MIMI_ERR_INVALID_ARG;
    char headers[512];
    build_http_headers(s, true, headers, sizeof(headers));

    mimi_http_request_t greq = {
        .method = "GET",
        .url = sse_url,
        .headers = headers,
        .body = NULL,
        .body_len = 0,
        .timeout_ms = 5000,
    };
    mimi_http_response_free(out_resp);
    memset(out_resp, 0, sizeof(*out_resp));
    return mimi_http_exec(&greq, out_resp);
}

static void mcp_legacy_apply_endpoint_info(mcp_server_t *s, const mcp_legacy_endpoint_info_t *info)
{
    if (!s || !info) return;
    if (info->message_url[0]) {
        strncpy(s->sse_message_url, info->message_url, sizeof(s->sse_message_url) - 1);
        s->sse_message_url[sizeof(s->sse_message_url) - 1] = '\0';
    }
    /* In legacy mode, some servers require the session id header for subsequent requests. */
    if (info->session_id[0] && !s->session_id[0]) {
        strncpy(s->session_id, info->session_id, sizeof(s->session_id) - 1);
        s->session_id[sizeof(s->session_id) - 1] = '\0';
    }
    s->http_mode = MCP_HTTP_MODE_LEGACY_HTTP_SSE;
}

static bool ensure_sse_endpoint(mcp_server_t *s, char *url_buf, size_t url_buf_sz)
{
    if (!s || !s->url[0]) return false;
    if (s->sse_message_url[0]) return true;
    if (s->http_mode == MCP_HTTP_MODE_STREAMABLE) return false;

    const char *sse_url = effective_url(s->url, url_buf, url_buf_sz);
    mimi_http_response_t gresp = {0};
    mimi_err_t gerr = mcp_legacy_fetch_endpoint_event(s, sse_url, &gresp);

    /* DashScope misconfiguration guard:
     * Users sometimes paste the non-SSE endpoint (trailing "/mcp") while selecting legacy SSE transport.
     * If the first GET fails, try replacing the trailing "/mcp" with "/sse" and retry once.
     */
    char alt_url_buf[768];
    const char *trailing_mcp = NULL;
    if (gerr != MIMI_OK || gresp.status >= 400 || !gresp.body || gresp.body_len == 0) {
        const char *u = sse_url;
        size_t ulen = u ? strlen(u) : 0;
        if (u && ulen >= 4 && strcmp(u + ulen - 4, "/mcp") == 0) trailing_mcp = u + ulen - 4;
    }

    if (trailing_mcp) {
        size_t prefix_len = (size_t)(trailing_mcp - sse_url);
        if (prefix_len + 4 + 1 < sizeof(alt_url_buf)) {
            memcpy(alt_url_buf, sse_url, prefix_len);
            memcpy(alt_url_buf + prefix_len, "/sse", 4);
            alt_url_buf[prefix_len + 4] = '\0';

            MIMI_LOGW(TAG, "SSE endpoint discovery retry: url=%s (from %s) server=%s",
                      alt_url_buf, sse_url, s->name);

            mimi_http_response_free(&gresp);
            memset(&gresp, 0, sizeof(gresp));
            gerr = mcp_legacy_fetch_endpoint_event(s, alt_url_buf, &gresp);
            sse_url = alt_url_buf;
        }
    }

    if (gerr != MIMI_OK) {
        MIMI_LOGE(TAG, "SSE endpoint discovery GET failed: err=%d server=%s", gerr, s->name);
        mimi_http_response_free(&gresp);
        return false;
    }

    if (gresp.status >= 400 || !gresp.body || gresp.body_len == 0) {
        MIMI_LOGE(TAG, "SSE endpoint discovery returned status=%d body_len=%zu", gresp.status, gresp.body_len);
        mimi_http_response_free(&gresp);
        return false;
    }
    mcp_legacy_endpoint_info_t info;
    bool parsed = mcp_legacy_parse_endpoint_event(&gresp, sse_url, &info);

    MIMI_LOGD(TAG, "SSE endpoint body (first 200 chars): %.*s",
              (int)(gresp.body_len > 200 ? 200 : gresp.body_len), (const char *)gresp.body);
    MIMI_LOGD(TAG, "SSE endpoint body_len=%zu status=%d full_hex: ", gresp.body_len, gresp.status);

    if (parsed && info.message_url[0]) {
        mcp_legacy_apply_endpoint_info(s, &info);
        MIMI_LOGD(TAG, "SSE endpoint discovered: url=%s", s->sse_message_url);
    } else {
        MIMI_LOGW(TAG, "Failed to parse SSE endpoint message, body starts with: %.*s",
                  (int)(gresp.body_len > 100 ? 100 : gresp.body_len), (const char *)gresp.body);
    }

    mimi_http_response_free(&gresp);
    return parsed && info.message_url[0] != '\0';
}

static void mcp_sse_build_poll_headers(mcp_server_t *s, char *out, size_t out_sz)
{
    if (!s || !out || out_sz == 0) return;
    int off = snprintf(out, out_sz,
                       "Accept: text/event-stream\r\n"
                       "Cache-Control: no-cache\r\n");
    if (off < 0) {
        out[0] = '\0';
        return;
    }
    if (s->extra_http_headers[0]) {
        size_t extra_len = strlen(s->extra_http_headers);
        if ((size_t)off + extra_len < out_sz) {
            memcpy(out + off, s->extra_http_headers, extra_len);
            out[off + extra_len] = '\0';
            off += (int)extra_len;
        }
    }
    if (s->last_event_id[0]) {
        size_t cur = strlen(out);
        snprintf(out + cur, out_sz - cur, "Last-Event-ID: %s\r\n", s->last_event_id);
    }
}

static char *sse_stream_until_match(mcp_server_t *s,
                                    const char *id_str,
                                    mcp_server_msg_cb_t on_notification,
                                    mcp_server_msg_cb_t on_request,
                                    uint32_t timeout_ms_total)
{
    if (!s || !id_str || !s->url[0]) return NULL;
    char sse_url_buf[768];
    const char *sse_url = effective_url(s->url, sse_url_buf, sizeof(sse_url_buf));
    if (!sse_url || !sse_url[0]) return NULL;

    mcp_sse_stream_wait_ctx_t *ctx =
        mcp_sse_stream_wait_start(s, sse_url, id_str, on_notification, on_request);
    char *ret = NULL;
    if (ctx) {
        mcp_sse_stream_wait_finish(ctx, timeout_ms_total, &ret);
    }
    return ret;
}

static const char *mcp_http_mode_name(mcp_http_mode_t mode)
{
    switch (mode) {
        case MCP_HTTP_MODE_STREAMABLE: return "streamable_http";
        case MCP_HTTP_MODE_LEGACY_HTTP_SSE: return "legacy_http_sse";
        case MCP_HTTP_MODE_UNKNOWN:
        default:
            return "unknown";
    }
}

static void mcp_http_mode_bootstrap(mcp_server_t *s)
{
    if (!s) return;
    if (s->http_mode == MCP_HTTP_MODE_UNKNOWN) {
        s->http_mode = MCP_HTTP_MODE_STREAMABLE;
    }
}

static bool mcp_should_fallback_streamable_to_legacy(mcp_server_t *s, int http_status, bool is_empty_response)
{
    /* Skip auto-fallback if transport type is explicitly configured:
     * - MCP_TRANSPORT_SSE = force legacy mode
     * - MCP_TRANSPORT_STREAMABLE_HTTP = force streamable mode
     * Only auto-detect mode (MCP_TRANSPORT_HTTP) allows fallback.
     */
    if (s->transport_type == MCP_TRANSPORT_SSE ||
        s->transport_type == MCP_TRANSPORT_STREAMABLE_HTTP) {
        return false;
    }
    
    /* Fallback triggers:
     * 400 = Bad Request (invalid request format)
     * 404 = Not Found (endpoint doesn't exist)
     * 405 = Method Not Allowed (POST not supported)
     * 409 = Conflict (protocol version/mode conflict, MCP spec)
     * 200 + empty response body = false positive detection (e.g. DashScope)
     */
    bool is_error_status = (http_status == 400 || http_status == 404 || 
                            http_status == 405 || http_status == 409);
    bool is_false_positive = (http_status >= 200 && http_status < 300 && is_empty_response);
    
    return is_error_status || is_false_positive;
}

static bool mcp_try_fallback_to_legacy(mcp_server_t *s, int http_status, bool is_empty_response)
{
    if (!s) return false;
    if (s->http_mode == MCP_HTTP_MODE_STREAMABLE &&
        mcp_should_fallback_streamable_to_legacy(s, http_status, is_empty_response)) {
        s->http_mode = MCP_HTTP_MODE_LEGACY_HTTP_SSE;
        s->sse_message_url[0] = '\0';
        s->last_event_id[0] = '\0';
        MIMI_LOGI(TAG, "HTTP transport fallback: streamable_http -> legacy_http_sse status=%d empty=%d server=%s",
                  http_status, is_empty_response, s->name);
        return true;
    }
    return false;
}

static const char *mcp_exchange_pick_req_url(mcp_server_t *s, char *sse_url_buf, size_t sse_url_buf_sz)
{
    if (!s) return NULL;
    if (s->http_mode == MCP_HTTP_MODE_STREAMABLE) return s->url;

    /* Legacy HTTP+SSE: POST to the SSE message URL (discovered via `event:endpoint`). */
    if (s->sse_message_url[0]) return s->sse_message_url;
    if (!ensure_sse_endpoint(s, sse_url_buf, sse_url_buf_sz)) return NULL;
    return s->sse_message_url[0] ? s->sse_message_url : NULL;
}

static mimi_err_t mcp_http_post_with_429_retry(const char *req_url,
                                               const char *headers,
                                               const char *request_json,
                                               mimi_http_response_t *out_resp)
{
    if (!req_url || !headers || !request_json || !out_resp) return MIMI_ERR_INVALID_ARG;

    mimi_err_t herr = MIMI_ERR_FAIL;
    int attempt_429 = 0;

    while (1) {
        mimi_http_request_t hreq = {
            .method = "POST",
            .url = req_url,
            .headers = headers,
            .body = (const uint8_t *)request_json,
            .body_len = strlen(request_json),
            .timeout_ms = MCP_HTTP_TIMEOUT_MS,
            .capture_response_headers = MCP_CAPTURE_HEADERS,
            .capture_response_headers_count = MCP_CAPTURE_HEADERS_COUNT,
        };

        mimi_http_response_free(out_resp);
        memset(out_resp, 0, sizeof(*out_resp));

        herr = mimi_http_exec(&hreq, out_resp);
        if (herr == MIMI_OK && out_resp->status == 429 && attempt_429 < MCP_HTTP_429_MAX_RETRIES) {
            uint32_t d = MCP_HTTP_429_BASE_DELAY_MS * (uint32_t)(1u << (uint32_t)attempt_429);
            attempt_429++;
            mimi_sleep_ms(d);
            continue;
        }
        break;
    }
    return herr;
}

static bool jsonrpc_extract_id_str(const char *json, char *out, size_t out_sz)
{
    if (!json || !out || out_sz == 0) return false;
    out[0] = '\0';
    const char *p = strstr(json, "\"id\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '"') {
        p++;
        const char *e = strchr(p, '"');
        if (!e) return false;
        size_t n = (size_t)(e - p);
        if (n == 0) return false;
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, p, n);
        out[n] = '\0';
        return true;
    } else {
        /* Numeric id */
        const char *e = p;
        while (*e && ((*e >= '0' && *e <= '9') || *e == '-' || *e == '.')) e++;
        size_t n = (size_t)(e - p);
        if (n == 0) return false;
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, p, n);
        out[n] = '\0';
        return true;
    }
}

static char *dashscope_empty_200_workaround(const char *request_json)
{
    if (!request_json) return NULL;

    /* DashScope MCP sometimes returns HTTP 200 with empty body for POST requests.
     * Create minimal JSON-RPC responses so the rest of the MCP pipeline can proceed. */
    char id[64];
    if (!jsonrpc_extract_id_str(request_json, id, sizeof(id))) {
        /* Fallback to "1" if id cannot be parsed */
        snprintf(id, sizeof(id), "1");
    }

    if (strstr(request_json, "\"method\":\"initialize\"") != NULL) {
        const char *tpl = "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":{}}";
        size_t n = (size_t)snprintf(NULL, 0, tpl, id);
        char *out = (char *)malloc(n + 1);
        if (!out) return NULL;
        snprintf(out, n + 1, tpl, id);
        return out;
    }

    /* tools/list can be either "tools/list" or "tools.list" depending on client */
    if (strstr(request_json, "\"method\":\"tools/list\"") != NULL ||
        strstr(request_json, "\"method\":\"tools.list\"") != NULL) {
        const char *tpl = "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":{\"tools\":[]}}";
        size_t n = (size_t)snprintf(NULL, 0, tpl, id);
        char *out = (char *)malloc(n + 1);
        if (!out) return NULL;
        snprintf(out, n + 1, tpl, id);
        return out;
    }

    return NULL;
}

char *mcp_http_exchange(mcp_server_t *s, const char *id_str, const char *request_json,
                        mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request)
{
    if (!s || !request_json || !id_str) return NULL;
    char headers[1024];
    char sse_url_buf[768];

    mcp_legacy_sse_pump_disarm(s);

    /* A single exchange can attempt streamable-first then legacy fallback.
     * Keep this loop explicit to avoid recursive calls that are harder to maintain. */
    for (int attempt = 0; attempt < 2; attempt++) {
        mcp_http_mode_bootstrap(s);

        const char *req_url = mcp_exchange_pick_req_url(s, sse_url_buf, sizeof(sse_url_buf));
        if (!req_url) {
            MIMI_LOGE(TAG, "HTTP exchange: failed to pick request URL server=%s mode=%d", s->name, (int)s->http_mode);
            return NULL;
        }
        MIMI_LOGD(TAG, "HTTP exchange mode: %s server=%s", mcp_http_mode_name(s->http_mode), s->name);

        /* Legacy HTTP+SSE: replies often arrive on SSE while POST is still in flight.
         * Arm expect_id before POST; block in wait after POST if body is still empty. */
        bool legacy_pump_ready = true;
        if (s->http_mode == MCP_HTTP_MODE_LEGACY_HTTP_SSE) {
            legacy_pump_ready = mcp_legacy_sse_pump_ensure(s, on_notification, on_request);
        }

        MIMI_LOGD(TAG, "HTTP exchange: server=%s id=%s url=%s body_len=%zu", s->name, id_str, req_url, strlen(request_json));
        MIMI_LOGD(TAG, "HTTP exchange body: %s", request_json);

        build_http_headers(s, false, headers, sizeof(headers));
        MIMI_LOGD(TAG, "HTTP exchange headers: %s", headers);

        /* Arm wait_id before POST: JSON-RPC reply may arrive on SSE while POST blocks. */
        if (legacy_pump_ready && s->http_mode == MCP_HTTP_MODE_LEGACY_HTTP_SSE) {
            mcp_legacy_sse_pump_arm(s, id_str);
        }

        mimi_http_response_t hresp = {0};
        mimi_err_t herr = mcp_http_post_with_429_retry(req_url, headers, request_json, &hresp);

        if (hresp.status == 429 || hresp.status == 404 || hresp.status >= 400) {
            MIMI_LOGI(TAG, "HTTP exchange: err=%d status=%d body_len=%zu content_type=%s server=%s",
                      herr, hresp.status, hresp.body_len,
                      hresp.content_type ? hresp.content_type : "(null)", s->name);
        } else {
            MIMI_LOGD(TAG, "HTTP exchange: err=%d status=%d body_len=%zu content_type=%s",
                      herr, hresp.status, hresp.body_len,
                      hresp.content_type ? hresp.content_type : "(null)");
        }
        if (hresp.body && hresp.body_len > 0) {
            size_t dump_len = hresp.body_len > 256 ? 256 : hresp.body_len;
            if (hresp.status == 429 || hresp.status == 404 || hresp.status >= 400) {
                MIMI_LOGI(TAG, "HTTP exchange body (first %zu bytes): %.*s",
                          dump_len, (int)dump_len, (const char *)hresp.body);
            } else {
                MIMI_LOGD(TAG, "HTTP exchange body (first %zu bytes): %.*s",
                          dump_len, (int)dump_len, (const char *)hresp.body);
            }
        }

        bool is_empty_response = (hresp.body_len == 0 || !hresp.body || !hresp.body[0]);

        /* DashScope compatibility:
         * Some legacy HTTP+SSE servers return HTTP 200 with an empty body for POST requests,
         * while delivering the actual JSON-RPC response asynchronously on the SSE stream.
         *
         * IMPORTANT: In legacy_http_sse mode we must try SSE wait FIRST, otherwise we
         * would incorrectly synthesize tools=[] and hide real tools (observed on DashScope).
         */
        if (herr == MIMI_OK && hresp.status >= 200 && hresp.status < 300 && is_empty_response) {
            if (s->http_mode == MCP_HTTP_MODE_LEGACY_HTTP_SSE) {
                MIMI_LOGI(TAG, "HTTP exchange: empty 2xx in legacy mode, waiting SSE response server=%s id=%s",
                          s->name, id_str);
                mimi_http_response_free(&hresp);
                char *ret = NULL;
                if (legacy_pump_ready) ret = mcp_legacy_sse_pump_wait_response(s, id_str, MCP_SSE_WAIT_TIMEOUT_MS);
                if (ret) {
                    mcp_legacy_sse_pump_disarm(s);
                    return ret;
                }

                /* Fallback: if SSE didn't deliver, synthesize minimal JSON-RPC responses
                 * for core methods so the MCP pipeline can proceed. */
                char *synthetic = dashscope_empty_200_workaround(request_json);
                if (synthetic) {
                    MIMI_LOGW(TAG, "HTTP exchange: SSE wait returned nothing; using synthetic response server=%s id=%s",
                              s->name, id_str);
                    mcp_legacy_sse_pump_disarm(s);
                    return synthetic;
                }
                mcp_legacy_sse_pump_disarm(s);
                return NULL;
            } else {
                /* Streamable mode: keep the previous behavior (some servers truly return nothing). */
                char *synthetic = dashscope_empty_200_workaround(request_json);
                if (synthetic) {
                    mimi_http_response_free(&hresp);
                    return synthetic;
                }
            }
        }
        
        /* False positive detection: HTTP client may return error for empty 200 responses,
         * but we need to check for fallback FIRST to handle servers that
         * incorrectly return empty success responses (e.g. DashScope)
         */
        if (hresp.status >= 200 && hresp.status < 300 && is_empty_response) {
            MIMI_LOGW(TAG, "Empty 200 response in streamable mode, checking fallback server=%s", s->name);
            if (mcp_try_fallback_to_legacy(s, hresp.status, is_empty_response)) {
                mimi_http_response_free(&hresp);
                mcp_legacy_sse_pump_disarm(s);
                continue;
            }
        }

        if (herr != MIMI_OK) {
            /* Also try fallback on HTTP error (some servers close connection with empty body,
             * which may cause HTTP client to report error even with 200 status)
             */
            if (hresp.status >= 200 && hresp.status < 300 &&
                mcp_try_fallback_to_legacy(s, hresp.status, is_empty_response)) {
                mimi_http_response_free(&hresp);
                mcp_legacy_sse_pump_disarm(s);
                continue;
            }
            MIMI_LOGE(TAG, "HTTP exchange failed: err=%d server=%s url=%s",
                      herr, s->name, req_url);
            mimi_http_response_free(&hresp);
            mcp_legacy_sse_pump_disarm(s);
            return NULL;
        }

        const char *session_id = mcp_session_id_from_resp(&hresp);
        if (session_id && session_id[0]) {
            strncpy(s->session_id, session_id, sizeof(s->session_id) - 1);
            s->session_id[sizeof(s->session_id) - 1] = '\0';
        }
        if (hresp.status == 404 && s->http_mode == MCP_HTTP_MODE_LEGACY_HTTP_SSE) {
            /* Legacy endpoint may expire quickly; force endpoint/session refresh
             * and retry once within this exchange loop. */
            s->initialized = false;
            s->session_id[0] = '\0';
            s->sse_message_url[0] = '\0';
            mimi_http_response_free(&hresp);
            mcp_legacy_sse_pump_disarm(s);
            continue;
        }

        if (hresp.status >= 400 || !hresp.body) {
            /* Streamable-first: if the server says the endpoint doesn't exist, fall back. */
            if (mcp_try_fallback_to_legacy(s, hresp.status, is_empty_response)) {
                mimi_http_response_free(&hresp);
                mcp_legacy_sse_pump_disarm(s);
                continue;
            }

            MIMI_LOGE(TAG, "HTTP exchange error: status=%d body_len=%zu server=%s url=%s",
                      hresp.status, hresp.body_len, s->name, req_url);
            mimi_http_response_free(&hresp);
            mcp_legacy_sse_pump_disarm(s);
            return NULL;
        }

        char *ret = NULL;
        bool is_sse = (hresp.content_type && strstr(hresp.content_type, "text/event-stream"));
        MIMI_LOGD(TAG, "HTTP exchange: is_sse=%d server=%s empty_response=%d", is_sse, s->name, is_empty_response);
        if (is_sse) {
            ret = parse_sse_for_response(s, (const char *)hresp.body, id_str, on_notification, on_request);
        } else {
            ret = process_jsonrpc_message(s, (const char *)hresp.body, id_str, on_notification, on_request);
        }
        if (!ret) {
            MIMI_LOGW(TAG, "HTTP exchange: no response parsed server=%s is_sse=%d", s->name, is_sse);
        }

        /* If POST didn't yield a JSON-RPC response, legacy may still deliver on SSE. */
        if (!ret && s->http_mode == MCP_HTTP_MODE_LEGACY_HTTP_SSE && legacy_pump_ready) {
            ret = mcp_legacy_sse_pump_wait_response(s, id_str, MCP_SSE_WAIT_TIMEOUT_MS);
        }

        /* Streamable HTTP: if POST returned only SSE priming/intermediate messages,
         * poll via GET with Last-Event-ID until we receive the matching response. */
        if (!ret && s->http_mode == MCP_HTTP_MODE_STREAMABLE && s->last_event_id[0]) {
            ret = sse_stream_until_match(s, id_str, on_notification, on_request, MCP_SSE_WAIT_TIMEOUT_MS);
        }

        mimi_http_response_free(&hresp);
        mcp_legacy_sse_pump_disarm(s);
        return ret;
    }
    return NULL;
}

mimi_err_t mcp_http_notify_post(mcp_server_t *s, const char *request_json)
{
    if (!s || !request_json) return MIMI_ERR_INVALID_ARG;
    char headers[1024];
    char sse_url_buf[768];

    for (int attempt = 0; attempt < 2; attempt++) {
        mcp_http_mode_bootstrap(s);
        const char *req_url = mcp_exchange_pick_req_url(s, sse_url_buf, sizeof(sse_url_buf));
        if (!req_url) {
            MIMI_LOGE(TAG, "HTTP notify: failed to pick request URL server=%s", s->name);
            return MIMI_ERR_FAIL;
        }

        build_http_headers(s, false, headers, sizeof(headers));
        mimi_http_request_t hreq = {
            .method = "POST",
            .url = req_url,
            .headers = headers,
            .body = (const uint8_t *)request_json,
            .body_len = strlen(request_json),
            .timeout_ms = MCP_HTTP_TIMEOUT_MS,
            .capture_response_headers = MCP_CAPTURE_HEADERS,
            .capture_response_headers_count = MCP_CAPTURE_HEADERS_COUNT,
        };
        mimi_http_response_t hresp = {0};
        mimi_err_t err = mimi_http_exec(&hreq, &hresp);
        if (err != MIMI_OK) {
            mimi_http_response_free(&hresp);
            return err;
        }
        const char *session_id = mcp_session_id_from_resp(&hresp);
        if (session_id && session_id[0]) {
            strncpy(s->session_id, session_id, sizeof(s->session_id) - 1);
            s->session_id[sizeof(s->session_id) - 1] = '\0';
        }
        if (hresp.status == 404 && s->session_id[0]) {
            s->initialized = false;
            s->session_id[0] = '\0';
        }

        bool is_empty_response = (hresp.body_len == 0 || !hresp.body || !hresp.body[0]);
        if (!mcp_try_fallback_to_legacy(s, hresp.status, is_empty_response)) {
            int status = hresp.status;
            bool ok = (status == 202 || (status >= 200 && status < 300));
            mimi_http_response_free(&hresp);
            if (!ok) MIMI_LOGW(TAG, "HTTP notify failed status=%d server=%s", status, s->name);
            return ok ? MIMI_OK : MIMI_ERR_FAIL;
        }
        mimi_http_response_free(&hresp);
    }
    return MIMI_ERR_FAIL;
}
