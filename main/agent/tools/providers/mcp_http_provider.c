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
    if (s->session_id[0]) {
        snprintf(headers, headers_sz,
                 "Content-Type: application/json\r\n"
                 "Accept: %s\r\n"
                 "MCP-Protocol-Version: %s\r\n"
                 "MCP-Session-Id: %s\r\n",
                 accept, pv, s->session_id);
    } else {
        snprintf(headers, headers_sz,
                 "Content-Type: application/json\r\n"
                 "Accept: %s\r\n"
                 "MCP-Protocol-Version: %s\r\n",
                 accept, pv);
    }
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

char *mcp_http_exchange(mcp_server_t *s, const char *id_str, const char *request_json,
                        mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request)
{
    if (!s || !request_json || !id_str) return NULL;
    MIMI_LOGD(TAG, "HTTP exchange: server=%s id=%s url=%s", s->name, id_str, s->url);
    char headers[1024];
    char url_buf[768];
    const char *req_url = effective_url(s->url, url_buf, sizeof(url_buf));
    build_http_headers(s, false, headers, sizeof(headers));

    mimi_http_request_t hreq = {
        .method = "POST",
        .url = req_url,
        .headers = headers,
        .body = (const uint8_t *)request_json,
        .body_len = strlen(request_json),
        .timeout_ms = MCP_HTTP_TIMEOUT_MS,
    };
    mimi_http_response_t hresp = {0};
    mimi_err_t herr = mimi_http_exec(&hreq, &hresp);
    MIMI_LOGD(TAG, "HTTP exchange: err=%d status=%d body_len=%zu content_type=%s",
              herr, hresp.status, hresp.body_len, hresp.content_type ? hresp.content_type : "(null)");
    if (herr != MIMI_OK) {
        mimi_http_response_free(&hresp);
        return NULL;
    }
    if (hresp.mcp_session_id && hresp.mcp_session_id[0]) {
        strncpy(s->session_id, hresp.mcp_session_id, sizeof(s->session_id) - 1);
        s->session_id[sizeof(s->session_id) - 1] = '\0';
    }
    if (hresp.status == 404 && s->session_id[0]) {
        s->initialized = false;
        s->session_id[0] = '\0';
        mimi_http_response_free(&hresp);
        return NULL;
    }
    if (hresp.status >= 400 || !hresp.body) {
        mimi_http_response_free(&hresp);
        return NULL;
    }

    char *ret = NULL;
    bool is_sse = (hresp.content_type && strstr(hresp.content_type, "text/event-stream"));
    if (is_sse) ret = parse_sse_for_response(s, (const char *)hresp.body, id_str, on_notification, on_request);
    else ret = process_jsonrpc_message(s, (const char *)hresp.body, id_str, on_notification, on_request);
    mimi_http_response_free(&hresp);

    if (!ret && is_sse) {
        /* Some MCP servers use SSE streams that may not include the matching response
         * in the initial POST body. Keep this fallback short to avoid blocking the
         * whole CLI turn for the full HTTP timeout window. */
        long long deadline = now_ms() + 2000;
        while (!ret && now_ms() < deadline) {
            char poll_headers[1024];
            build_http_headers(s, true, poll_headers, sizeof(poll_headers));
            if (s->last_event_id[0]) {
                size_t cur = strlen(poll_headers);
                snprintf(poll_headers + cur, sizeof(poll_headers) - cur,
                         "Last-Event-ID: %s\r\n", s->last_event_id);
            }
            mimi_http_request_t greq = {
                .method = "GET",
                .url = req_url,
                .headers = poll_headers,
                .body = NULL,
                .body_len = 0,
                .timeout_ms = 1000,
            };
            mimi_http_response_t gresp = {0};
            mimi_err_t gerr = mimi_http_exec(&greq, &gresp);
            if (gerr != MIMI_OK) {
                mimi_http_response_free(&gresp);
                break;
            }
            if (gresp.mcp_session_id && gresp.mcp_session_id[0]) {
                strncpy(s->session_id, gresp.mcp_session_id, sizeof(s->session_id) - 1);
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
            bool g_is_sse = (gresp.content_type && strstr(gresp.content_type, "text/event-stream"));
            if (g_is_sse) ret = parse_sse_for_response(s, (const char *)gresp.body, id_str, on_notification, on_request);
            else ret = process_jsonrpc_message(s, (const char *)gresp.body, id_str, on_notification, on_request);
            mimi_http_response_free(&gresp);
            if (!ret) mimi_sleep_ms((uint32_t)(s->sse_retry_ms > 0 ? s->sse_retry_ms : 1000));
        }
    }
    return ret;
}

mimi_err_t mcp_http_notify_post(mcp_server_t *s, const char *request_json)
{
    if (!s || !request_json) return MIMI_ERR_INVALID_ARG;
    char headers[1024];
    char url_buf[768];
    const char *req_url = effective_url(s->url, url_buf, sizeof(url_buf));
    build_http_headers(s, false, headers, sizeof(headers));
    mimi_http_request_t hreq = {
        .method = "POST",
        .url = req_url,
        .headers = headers,
        .body = (const uint8_t *)request_json,
        .body_len = strlen(request_json),
        .timeout_ms = MCP_HTTP_TIMEOUT_MS,
    };
    mimi_http_response_t hresp = {0};
    mimi_err_t err = mimi_http_exec(&hreq, &hresp);
    if (err != MIMI_OK) {
        mimi_http_response_free(&hresp);
        return err;
    }
    if (hresp.mcp_session_id && hresp.mcp_session_id[0]) {
        strncpy(s->session_id, hresp.mcp_session_id, sizeof(s->session_id) - 1);
        s->session_id[sizeof(s->session_id) - 1] = '\0';
    }
    if (hresp.status == 404 && s->session_id[0]) {
        s->initialized = false;
        s->session_id[0] = '\0';
    }
    int status = hresp.status;
    bool ok = (status == 202 || (status >= 200 && status < 300));
    mimi_http_response_free(&hresp);
    if (!ok) MIMI_LOGW(TAG, "HTTP notify failed status=%d server=%s", status, s->name);
    return ok ? MIMI_OK : MIMI_ERR_FAIL;
}
