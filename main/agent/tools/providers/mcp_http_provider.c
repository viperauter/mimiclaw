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
static const int MCP_HTTP_429_MAX_RETRIES = 2;
static const uint32_t MCP_HTTP_429_BASE_DELAY_MS = 800;

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
    build_http_headers(s, true, out, out_sz);
    if (s->last_event_id[0]) {
        size_t cur = strlen(out);
        snprintf(out + cur, out_sz - cur, "Last-Event-ID: %s\r\n", s->last_event_id);
    }
}

static mimi_err_t mcp_sse_poll_get_once(mcp_server_t *s, const char *sse_url, mimi_http_response_t *out_resp)
{
    if (!s || !sse_url || !out_resp) return MIMI_ERR_INVALID_ARG;
    char poll_headers[1024];
    mcp_sse_build_poll_headers(s, poll_headers, sizeof(poll_headers));

    mimi_http_request_t greq = {
        .method = "GET",
        .url = sse_url,
        .headers = poll_headers,
        .body = NULL,
        .body_len = 0,
        .timeout_ms = 5000,
        .capture_response_headers = MCP_CAPTURE_HEADERS,
        .capture_response_headers_count = MCP_CAPTURE_HEADERS_COUNT,
    };
    mimi_http_response_free(out_resp);
    memset(out_resp, 0, sizeof(*out_resp));
    return mimi_http_exec(&greq, out_resp);
}

static char *mcp_sse_parse_and_dispatch(mcp_server_t *s,
                                        const mimi_http_response_t *resp,
                                        const char *id_str,
                                        mcp_server_msg_cb_t on_notification,
                                        mcp_server_msg_cb_t on_request)
{
    if (!s || !resp || !id_str || !resp->body) return NULL;
    bool is_sse = (resp->content_type && strstr(resp->content_type, "text/event-stream"));
    if (is_sse) return parse_sse_for_response(s, (const char *)resp->body, id_str, on_notification, on_request);
    return process_jsonrpc_message(s, (const char *)resp->body, id_str, on_notification, on_request);
}

static char *sse_poll_until_match(mcp_server_t *s, const char *id_str,
                                   mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request,
                                   uint32_t timeout_ms_total)
{
    if (!s || !id_str) return NULL;
    if (!s->url[0]) return NULL;

    char sse_url_buf[768];
    const char *sse_url = effective_url(s->url, sse_url_buf, sizeof(sse_url_buf));
    if (!sse_url || !sse_url[0]) return NULL;

    long long deadline = now_ms() + (long long)timeout_ms_total;
    while (now_ms() < deadline) {
        mimi_http_response_t gresp = {0};
        mimi_err_t gerr = mcp_sse_poll_get_once(s, sse_url, &gresp);
        if (gerr != MIMI_OK) {
            MIMI_LOGW(TAG, "SSE poll GET failed: err=%d server=%s url=%s",
                      gerr, s->name, sse_url);
            mimi_http_response_free(&gresp);
            break;
        }
        MIMI_LOGD(TAG, "SSE poll GET: status=%d body_len=%zu content_type=%s",
                  gresp.status, gresp.body_len,
                  gresp.content_type ? gresp.content_type : "(null)");
        if (gresp.body && gresp.body_len > 0) {
            size_t dump_len = gresp.body_len > 256 ? 256 : gresp.body_len;
            MIMI_LOGD(TAG, "SSE poll GET body (first %zu bytes): %.*s",
                      dump_len, (int)dump_len, (const char *)gresp.body);
        }

        const char *session_id = mcp_session_id_from_resp(&gresp);
        if (session_id && session_id[0]) {
            strncpy(s->session_id, session_id, sizeof(s->session_id) - 1);
            s->session_id[sizeof(s->session_id) - 1] = '\0';
        }
        if (gresp.status == 404 && s->session_id[0]) {
            s->initialized = false;
            s->session_id[0] = '\0';
            mimi_http_response_free(&gresp);
            break;
        }
        if (gresp.status >= 400 || !gresp.body) {
            mimi_http_response_free(&gresp);
            break;
        }

        char *ret = mcp_sse_parse_and_dispatch(s, &gresp, id_str, on_notification, on_request);

        mimi_http_response_free(&gresp);
        if (ret) return ret;

        mimi_sleep_ms((uint32_t)(s->sse_retry_ms > 0 ? s->sse_retry_ms : 1000));
    }
    return NULL;
}

typedef struct {
    mcp_server_t *s;
    char *id_str; /* owned copy */
    mcp_server_msg_cb_t on_notification;
    mcp_server_msg_cb_t on_request;
    mimi_mutex_t *mu;
    mimi_cond_t *cv;
    bool done;
    char *ret; /* owned result; free by waiter if not returned */
} mcp_sse_wait_ctx_t;

static void mcp_sse_wait_task(void *arg)
{
    mcp_sse_wait_ctx_t *ctx = (mcp_sse_wait_ctx_t *)arg;
    if (!ctx || !ctx->s || !ctx->id_str) return;
    char *ret = sse_poll_until_match(ctx->s, ctx->id_str, ctx->on_notification, ctx->on_request,
                                     MCP_SSE_WAIT_TIMEOUT_MS);
    if (mimi_mutex_lock(ctx->mu) == MIMI_OK) {
        ctx->ret = ret;
        ctx->done = true;
        (void)mimi_cond_signal(ctx->cv);
        (void)mimi_mutex_unlock(ctx->mu);
    }
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

static mcp_sse_wait_ctx_t *mcp_legacy_create_sse_wait_ctx(mcp_server_t *s, const char *id_str,
                                                           mcp_server_msg_cb_t on_notification,
                                                           mcp_server_msg_cb_t on_request)
{
    if (!s || !id_str) return NULL;
    mcp_sse_wait_ctx_t *ctx = (mcp_sse_wait_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->s = s;
    ctx->id_str = strdup(id_str);
    ctx->on_notification = on_notification;
    ctx->on_request = on_request;
    ctx->done = false;
    ctx->ret = NULL;

    if (!ctx->id_str) {
        free(ctx);
        return NULL;
    }

    ctx->mu = NULL;
    ctx->cv = NULL;
    if (mimi_mutex_create(&ctx->mu) != MIMI_OK || mimi_cond_create(&ctx->cv) != MIMI_OK) {
        if (ctx->cv) mimi_cond_destroy(ctx->cv);
        if (ctx->mu) mimi_mutex_destroy(ctx->mu);
        free(ctx->id_str);
        free(ctx);
        return NULL;
    }

    (void)mimi_task_create_detached("mcp_sse_wait", mcp_sse_wait_task, ctx);
    return ctx;
}

static void mcp_legacy_finish_sse_wait_ctx(mcp_sse_wait_ctx_t *sse_ctx, char **inout_ret)
{
    if (!sse_ctx || !inout_ret) return;

    long long start_wait = now_ms();
    uint32_t timeout_ms = MCP_HTTP_TIMEOUT_MS;

    (void)mimi_mutex_lock(sse_ctx->mu);
    while (!sse_ctx->done) {
        long long elapsed = now_ms() - start_wait;
        if ((uint32_t)elapsed >= timeout_ms) break;
        (void)mimi_cond_wait(sse_ctx->cv, sse_ctx->mu, timeout_ms - (uint32_t)elapsed);
    }
    (void)mimi_mutex_unlock(sse_ctx->mu);

    /* If POST already returned response, keep it and drop SSE duplicate. */
    if (!*inout_ret && sse_ctx->ret) {
        *inout_ret = sse_ctx->ret;
        sse_ctx->ret = NULL;
    } else if (*inout_ret && sse_ctx->ret) {
        free(sse_ctx->ret);
        sse_ctx->ret = NULL;
    }

    free(sse_ctx->id_str);
    mimi_cond_destroy(sse_ctx->cv);
    mimi_mutex_destroy(sse_ctx->mu);
    free(sse_ctx);
}

static bool mcp_should_fallback_streamable_to_legacy(int http_status)
{
    return http_status == 400 || http_status == 404 || http_status == 405;
}

static bool mcp_try_fallback_to_legacy(mcp_server_t *s, int http_status)
{
    if (!s) return false;
    if (s->http_mode == MCP_HTTP_MODE_STREAMABLE &&
        mcp_should_fallback_streamable_to_legacy(http_status)) {
        s->http_mode = MCP_HTTP_MODE_LEGACY_HTTP_SSE;
        s->sse_message_url[0] = '\0';
        s->last_event_id[0] = '\0';
        MIMI_LOGI(TAG, "HTTP transport fallback: streamable_http -> legacy_http_sse status=%d server=%s",
                  http_status, s->name);
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

char *mcp_http_exchange(mcp_server_t *s, const char *id_str, const char *request_json,
                        mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request)
{
    if (!s || !request_json || !id_str) return NULL;
    char headers[1024];
    char sse_url_buf[768];

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

        /* Start a concurrent SSE reader before POST only in legacy mode.
         * Streamable HTTP servers MUST NOT send unrelated responses on GET streams. */
        mcp_sse_wait_ctx_t *sse_ctx = NULL;
        if (s->http_mode == MCP_HTTP_MODE_LEGACY_HTTP_SSE) {
            sse_ctx = mcp_legacy_create_sse_wait_ctx(s, id_str, on_notification, on_request);
        }

        MIMI_LOGD(TAG, "HTTP exchange: server=%s id=%s url=%s body_len=%zu", s->name, id_str, req_url, strlen(request_json));
        MIMI_LOGD(TAG, "HTTP exchange body: %s", request_json);

        build_http_headers(s, false, headers, sizeof(headers));
        MIMI_LOGD(TAG, "HTTP exchange headers: %s", headers);

        mimi_http_response_t hresp = {0};
        mimi_err_t herr = mcp_http_post_with_429_retry(req_url, headers, request_json, &hresp);

        MIMI_LOGD(TAG, "HTTP exchange: err=%d status=%d body_len=%zu content_type=%s",
                  herr, hresp.status, hresp.body_len,
                  hresp.content_type ? hresp.content_type : "(null)");
        if (hresp.body && hresp.body_len > 0) {
            size_t dump_len = hresp.body_len > 256 ? 256 : hresp.body_len;
            MIMI_LOGD(TAG, "HTTP exchange body (first %zu bytes): %.*s",
                      dump_len, (int)dump_len, (const char *)hresp.body);
        }

        if (herr != MIMI_OK) {
            MIMI_LOGE(TAG, "HTTP exchange failed: err=%d server=%s url=%s",
                      herr, s->name, s->url);
            mimi_http_response_free(&hresp);
            return NULL;
        }

        const char *session_id = mcp_session_id_from_resp(&hresp);
        if (session_id && session_id[0]) {
            strncpy(s->session_id, session_id, sizeof(s->session_id) - 1);
            s->session_id[sizeof(s->session_id) - 1] = '\0';
        }
        if (hresp.status == 404 && s->session_id[0]) {
            s->initialized = false;
            s->session_id[0] = '\0';
            mimi_http_response_free(&hresp);
            return NULL;
        }

        if (hresp.status >= 400 || !hresp.body) {
            /* Streamable-first: if the server says the endpoint doesn't exist, fall back. */
            if (mcp_try_fallback_to_legacy(s, hresp.status)) {
                mimi_http_response_free(&hresp);
                continue;
            }

            MIMI_LOGE(TAG, "HTTP exchange error: status=%d body_len=%zu server=%s url=%s",
                      hresp.status, hresp.body_len, s->name, req_url);
            mimi_http_response_free(&hresp);
            return NULL;
        }

        char *ret = NULL;
        bool is_sse = (hresp.content_type && strstr(hresp.content_type, "text/event-stream"));
        MIMI_LOGD(TAG, "HTTP exchange: is_sse=%d server=%s", is_sse, s->name);
        if (is_sse) {
            ret = parse_sse_for_response(s, (const char *)hresp.body, id_str, on_notification, on_request);
        } else {
            ret = process_jsonrpc_message(s, (const char *)hresp.body, id_str, on_notification, on_request);
        }
        if (!ret) {
            MIMI_LOGW(TAG, "HTTP exchange: no response parsed server=%s is_sse=%d", s->name, is_sse);
        }

        /* Streamable HTTP: if POST returned only SSE priming/intermediate messages,
         * poll via GET with Last-Event-ID until we receive the matching response. */
        if (!ret && s->http_mode == MCP_HTTP_MODE_STREAMABLE && s->last_event_id[0]) {
            ret = sse_poll_until_match(s, id_str, on_notification, on_request, MCP_SSE_WAIT_TIMEOUT_MS);
        }

        mimi_http_response_free(&hresp);

        if (sse_ctx) mcp_legacy_finish_sse_wait_ctx(sse_ctx, &ret);
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

        if (!mcp_try_fallback_to_legacy(s, hresp.status)) {
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
