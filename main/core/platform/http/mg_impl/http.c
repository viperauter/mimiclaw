#include "../http.h"
#include "../../log.h"
#include "../../runtime.h"
#include "../../os/os.h"
#include "../../event/event_bus.h"
#include "../../event/event_dispatcher.h"

#include "mongoose.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <arpa/inet.h>
#endif

#include "proxy/http_proxy.h"

typedef enum {
    HTTP_PHASE_DIRECT = 0,
    HTTP_PHASE_PROXY_HANDSHAKE,
    HTTP_PHASE_HTTP_ACTIVE,
} http_phase_t;

typedef struct {
    const mimi_http_request_t *req;
    mimi_http_response_t *resp;
    mimi_err_t result;
    bool done;
    bool closed;
    char host_name[128];
    bool use_proxy;
    http_phase_t phase;
    mimi_mutex_t *mutex;
    mimi_cond_t *cond;
} http_ctx_t;

/* Async HTTP request context (owned by event loop thread, freed on MG_EV_CLOSE) */
typedef struct {
    mimi_http_request_t req;          /* owned deep copy */
    mimi_http_response_t *resp;       /* provided by caller, must outlive callback */
    mimi_http_callback_t callback;
    void *callback_data;
    mimi_err_t result;
    bool done;
    bool posted;
    bool use_proxy;
    http_phase_t phase;
    char host_name[128];
    struct mg_mgr *mgr;
    struct mg_connection *conn;
    struct mg_timer *timeout_timer;
    event_bus_t *bus;
} http_async_req_t;

typedef enum {
    HTTP_INTERNAL_ASYNC_COMPLETE = 1,
    HTTP_INTERNAL_STREAM_EVENT = 2,
} http_internal_msg_type_t;

typedef struct {
    http_internal_msg_type_t type;
} http_internal_msg_base_t;

typedef enum {
    HTTP_STREAM_EVENT_OPEN = 1,
    HTTP_STREAM_EVENT_SSE = 2,
    HTTP_STREAM_EVENT_DATA = 3,
    HTTP_STREAM_EVENT_CLOSE = 4,
} http_stream_event_kind_t;

typedef struct {
    http_internal_msg_type_t type;
    mimi_err_t result;
    mimi_http_response_t *resp;
    mimi_http_callback_t callback;
    void *callback_data;
} http_internal_async_complete_t;

typedef struct mimi_http_stream_handle {
    atomic_bool close_requested;
    atomic_int refcnt;
} mimi_http_stream_handle_t;

static void http_stream_handle_release(mimi_http_stream_handle_t *h)
{
    if (!h) return;
    if (atomic_fetch_sub_explicit(&h->refcnt, 1, memory_order_acq_rel) == 1) {
        free(h);
    }
}

typedef struct {
    http_internal_msg_type_t type;
    http_stream_event_kind_t kind;
    mimi_http_stream_callbacks_t callbacks;
    void *user_data;
    mimi_err_t err;
    mimi_http_response_t open_meta;
    char *event_name;
    char *event_data;
    char *event_id;
    int retry_ms;
    uint8_t *data;
    size_t data_len;
} http_stream_event_msg_t;

/* Body framing after response headers (mg_connect raw path; avoids mongoose http_cb
 * waiting for a terminal chunked chunk on infinite SSE). */
typedef enum {
    HTTP_STREAM_BODY_CHUNKED = 1,
    HTTP_STREAM_BODY_FIXED_LEN = 2,
    HTTP_STREAM_BODY_RAW = 3,
} http_stream_body_mode_t;

typedef struct {
    mimi_http_request_t req;      /* owned deep copy */
    mimi_http_stream_callbacks_t callbacks;
    void *user_data;
    mimi_err_t result;
    bool done;
    bool posted_close;
    bool opened;
    bool is_sse;
    bool headers_parsed;
    uint8_t body_mode; /* http_stream_body_mode_t */
    size_t fixed_remain;
    char host_name[128];
    struct mg_mgr *mgr;
    struct mg_connection *conn;
    struct mg_timer *timeout_timer;
    event_bus_t *bus;
    mimi_http_stream_handle_t *handle;
    mimi_http_response_t open_meta;
    char *sse_buf;
    size_t sse_buf_len;
    size_t sse_buf_cap;
    uint32_t connect_timeout_ms;
    uint32_t read_idle_ms;
} http_stream_req_t;

static void http_stream_rearm_read_idle(http_stream_req_t *ctx);

static mimi_err_t http_stream_open_internal(const mimi_http_request_t *req,
                                            const mimi_http_stream_callbacks_t *callbacks,
                                            void *user_data,
                                            mimi_http_stream_handle_t **out_handle);

static void http_send_request(struct mg_connection *c, http_ctx_t *ctx)
{
    struct mg_str host = mg_url_host(ctx->req->url);
    const char *uri = mg_url_uri(ctx->req->url);
    if (!uri) uri = "/";
    int port = mg_url_port(ctx->req->url);
    bool is_ssl = mg_url_is_ssl(ctx->req->url);
    if (port == 0) port = is_ssl ? 443 : 80;

    snprintf(ctx->host_name, sizeof(ctx->host_name), "%.*s",
             (int) host.len, host.buf ? host.buf : "");
    char host_header[192];
    bool default_port = (is_ssl && port == 443) || (!is_ssl && port == 80);
    if (default_port) snprintf(host_header, sizeof(host_header), "%s", ctx->host_name);
    else snprintf(host_header, sizeof(host_header), "%s:%d", ctx->host_name, port);

    const char *extra = (ctx->req->headers && ctx->req->headers[0]) ? ctx->req->headers : "";
    size_t body_len = ctx->req->body ? ctx->req->body_len : 0;

    mg_printf(c,
              "%s %s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "Connection: keep-alive\r\n"
              "%s%s"
              "Content-Length: %lu\r\n"
              "\r\n",
              ctx->req->method ? ctx->req->method : "GET",
              uri,
              host_header,
              extra,
              (extra[0] && (extra[strlen(extra) - 1] != '\n')) ? "\r\n" : "",
              (unsigned long)body_len);

    if (body_len > 0) {
        (void)mg_send(c, ctx->req->body, body_len);
    }
}

static void http_async_free_owned(http_async_req_t *ctx)
{
    if (!ctx) return;
    free((void *)ctx->req.method);
    free((void *)ctx->req.url);
    free((void *)ctx->req.headers);
    if (ctx->req.capture_response_headers) {
        for (size_t i = 0; i < ctx->req.capture_response_headers_count; i++) {
            free((void *)ctx->req.capture_response_headers[i]);
            ctx->req.capture_response_headers[i] = NULL;
        }
        free((void *)ctx->req.capture_response_headers);
    }
    free((void *)ctx->req.body);
    ctx->req.method = NULL;
    ctx->req.url = NULL;
    ctx->req.headers = NULL;
    ctx->req.capture_response_headers = NULL;
    ctx->req.capture_response_headers_count = 0;
    ctx->req.body = NULL;
    ctx->req.body_len = 0;
}

static void http_async_destroy_timeout_timer(http_async_req_t *ctx)
{
    if (!ctx || !ctx->timeout_timer) return;
    /* During shutdown, mg_mgr_free() frees all timers, sets mgr->timers = NULL,
     * and only then triggers MG_EV_CLOSE via mg_mgr_poll(). In that path we must
     * not touch or free timers here (otherwise we risk double-free), and we must
     * also avoid calling runtime APIs that take locks (MG_EV_CLOSE may run while
     * runtime holds its state mutex during deinit). */
    if (ctx->mgr && ctx->mgr->timers == NULL) {
        ctx->timeout_timer = NULL;
        return;
    }
    if (ctx->mgr) mg_timer_free(&ctx->mgr->timers, ctx->timeout_timer);
    free(ctx->timeout_timer);
    ctx->timeout_timer = NULL;
}

static void http_async_post_complete(http_async_req_t *ctx)
{
    if (!ctx || ctx->posted) return;
    ctx->posted = true;

    if (!ctx->callback) return;

    event_bus_t *bus = event_bus_get_global();
    if (!bus) return;

    http_internal_async_complete_t *c = (http_internal_async_complete_t *)calloc(1, sizeof(*c));
    if (!c) return;
    c->type = HTTP_INTERNAL_ASYNC_COMPLETE;
    c->result = ctx->result;
    c->resp = ctx->resp;
    c->callback = ctx->callback;
    c->callback_data = ctx->callback_data;

    /* Deliver completion on dispatcher workers via internal recv event */
    int post_rc = event_bus_post_recv(bus,
                              EVENT_RECV,
                              0,
                              CONN_HTTP_CLIENT,
                              NULL,
                              CONN_TO_ID(c),
                              EVENT_FLAG_INTERNAL);
    if (post_rc != 0) {
        /* Shutdown or queue full: avoid leaking the completion record. */
        free(c);
    }
}

static void http_async_finish(http_async_req_t *ctx, mimi_err_t result)
{
    if (!ctx || ctx->done) return;
    ctx->done = true;
    ctx->result = result;
    /* Stop timeout timer as soon as we know the final outcome. */
    http_async_destroy_timeout_timer(ctx);
    if (ctx->conn) ctx->conn->is_closing = 1;
}

static void http_async_timeout_cb(void *arg)
{
    http_async_req_t *ctx = (http_async_req_t *)arg;
    if (!ctx || ctx->done) return;
    http_async_finish(ctx, MIMI_ERR_TIMEOUT);
}

static void http_stream_free_open_meta(mimi_http_response_t *meta)
{
    if (!meta) return;
    free(meta->content_type);
    meta->content_type = NULL;
    if (meta->captured_headers) {
        for (size_t i = 0; i < meta->captured_headers_count; i++) {
            free(meta->captured_headers[i].name);
            free(meta->captured_headers[i].value);
        }
        free(meta->captured_headers);
    }
    meta->captured_headers = NULL;
    meta->captured_headers_count = 0;
}

static void http_stream_free_owned(http_stream_req_t *ctx)
{
    if (!ctx) return;
    free((void *)ctx->req.method);
    free((void *)ctx->req.url);
    free((void *)ctx->req.headers);
    free((void *)ctx->req.body);
    if (ctx->req.capture_response_headers) {
        for (size_t i = 0; i < ctx->req.capture_response_headers_count; i++) {
            free((void *)ctx->req.capture_response_headers[i]);
        }
        free((void *)ctx->req.capture_response_headers);
    }
    ctx->req.method = NULL;
    ctx->req.url = NULL;
    ctx->req.headers = NULL;
    ctx->req.body = NULL;
    ctx->req.capture_response_headers = NULL;
    ctx->req.capture_response_headers_count = 0;
    free(ctx->sse_buf);
    ctx->sse_buf = NULL;
    ctx->sse_buf_len = 0;
    ctx->sse_buf_cap = 0;
    http_stream_free_open_meta(&ctx->open_meta);
}

static void http_stream_destroy_timeout_timer(http_stream_req_t *ctx)
{
    if (!ctx || !ctx->timeout_timer) return;
    if (ctx->mgr && ctx->mgr->timers == NULL) {
        ctx->timeout_timer = NULL;
        return;
    }
    if (ctx->mgr) mg_timer_free(&ctx->mgr->timers, ctx->timeout_timer);
    free(ctx->timeout_timer);
    ctx->timeout_timer = NULL;
}

static void http_stream_post_event(http_stream_req_t *ctx, http_stream_event_msg_t *evt)
{
    if (!ctx || !ctx->bus || !evt) return;
    int post_rc = event_bus_post_recv(ctx->bus,
                                      EVENT_RECV,
                                      0,
                                      CONN_HTTP_CLIENT,
                                      NULL,
                                      CONN_TO_ID(evt),
                                      EVENT_FLAG_INTERNAL);
    if (post_rc != 0) {
        http_stream_free_open_meta(&evt->open_meta);
        free(evt->event_name);
        free(evt->event_data);
        free(evt->data);
        free(evt);
    }
}

static void http_stream_post_open(http_stream_req_t *ctx)
{
    if (!ctx || ctx->opened) return;
    ctx->opened = true;
    /* Connect phase used `connect_timeout_ms`; after headers, use read-idle (reset per chunk). */
    http_stream_rearm_read_idle(ctx);
    if (!ctx->callbacks.on_open) return;

    http_stream_event_msg_t *evt = (http_stream_event_msg_t *)calloc(1, sizeof(*evt));
    if (!evt) return;
    evt->type = HTTP_INTERNAL_STREAM_EVENT;
    evt->kind = HTTP_STREAM_EVENT_OPEN;
    evt->callbacks = ctx->callbacks;
    evt->user_data = ctx->user_data;
    evt->open_meta.status = ctx->open_meta.status;
    if (ctx->open_meta.content_type) {
        evt->open_meta.content_type = strdup(ctx->open_meta.content_type);
    }
    evt->open_meta.captured_headers_count = ctx->open_meta.captured_headers_count;
    if (ctx->open_meta.captured_headers_count > 0 && ctx->open_meta.captured_headers) {
        evt->open_meta.captured_headers = (mimi_http_header_kv_t *)calloc(
            ctx->open_meta.captured_headers_count, sizeof(mimi_http_header_kv_t));
        if (!evt->open_meta.captured_headers) {
            free(evt->open_meta.content_type);
            free(evt);
            return;
        }
        for (size_t i = 0; i < ctx->open_meta.captured_headers_count; i++) {
            if (ctx->open_meta.captured_headers[i].name) {
                evt->open_meta.captured_headers[i].name =
                    strdup(ctx->open_meta.captured_headers[i].name);
            }
            if (ctx->open_meta.captured_headers[i].value) {
                evt->open_meta.captured_headers[i].value =
                    strdup(ctx->open_meta.captured_headers[i].value);
            }
        }
    }
    http_stream_post_event(ctx, evt);
}

static void http_stream_post_sse_ex(http_stream_req_t *ctx,
                                    const char *event_name,
                                    const char *data,
                                    const char *event_id,
                                    int retry_ms)
{
    if (!ctx || (!ctx->callbacks.on_sse_event && !ctx->callbacks.on_sse_event_ex) || !data) return;
    http_stream_event_msg_t *evt = (http_stream_event_msg_t *)calloc(1, sizeof(*evt));
    if (!evt) return;
    evt->type = HTTP_INTERNAL_STREAM_EVENT;
    evt->kind = HTTP_STREAM_EVENT_SSE;
    evt->callbacks = ctx->callbacks;
    evt->user_data = ctx->user_data;
    evt->retry_ms = retry_ms;
    if (event_name && event_name[0]) evt->event_name = strdup(event_name);
    if (event_id && event_id[0]) evt->event_id = strdup(event_id);
    evt->event_data = strdup(data);
    if (!evt->event_data) {
        free(evt->event_name);
        free(evt->event_id);
        free(evt);
        return;
    }
    http_stream_post_event(ctx, evt);
}

static void http_stream_post_data(http_stream_req_t *ctx, const uint8_t *data, size_t len)
{
    if (!ctx || !ctx->callbacks.on_data || !data || len == 0) return;
    http_stream_event_msg_t *evt = (http_stream_event_msg_t *)calloc(1, sizeof(*evt));
    if (!evt) return;
    evt->type = HTTP_INTERNAL_STREAM_EVENT;
    evt->kind = HTTP_STREAM_EVENT_DATA;
    evt->callbacks = ctx->callbacks;
    evt->user_data = ctx->user_data;
    evt->data = (uint8_t *)malloc(len);
    if (!evt->data) {
        free(evt);
        return;
    }
    memcpy(evt->data, data, len);
    evt->data_len = len;
    http_stream_post_event(ctx, evt);
}

static void http_stream_post_close(http_stream_req_t *ctx)
{
    if (!ctx || ctx->posted_close || !ctx->callbacks.on_close) return;
    ctx->posted_close = true;
    http_stream_event_msg_t *evt = (http_stream_event_msg_t *)calloc(1, sizeof(*evt));
    if (!evt) return;
    evt->type = HTTP_INTERNAL_STREAM_EVENT;
    evt->kind = HTTP_STREAM_EVENT_CLOSE;
    evt->callbacks = ctx->callbacks;
    evt->user_data = ctx->user_data;
    evt->err = ctx->result;
    http_stream_post_event(ctx, evt);
}

static mimi_err_t http_stream_sse_append(http_stream_req_t *ctx, const void *chunk, size_t len)
{
    if (!ctx || !chunk || len == 0) return MIMI_OK;
    size_t need = ctx->sse_buf_len + len + 1;
    if (need > ctx->sse_buf_cap) {
        size_t cap = ctx->sse_buf_cap ? ctx->sse_buf_cap : 1024;
        while (cap < need) cap *= 2;
        char *nb = (char *)realloc(ctx->sse_buf, cap);
        if (!nb) return MIMI_ERR_NO_MEM;
        ctx->sse_buf = nb;
        ctx->sse_buf_cap = cap;
    }
    memcpy(ctx->sse_buf + ctx->sse_buf_len, chunk, len);
    ctx->sse_buf_len += len;
    ctx->sse_buf[ctx->sse_buf_len] = '\0';
    return MIMI_OK;
}

static size_t http_stream_find_event_delim(const char *buf, size_t len, size_t *delim_len)
{
    if (!buf || len == 0) return (size_t)-1;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            if (delim_len) *delim_len = 2;
            return i;
        }
        if (i + 3 < len &&
            buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            if (delim_len) *delim_len = 4;
            return i;
        }
    }
    return (size_t)-1;
}

static void http_stream_parse_sse_events(http_stream_req_t *ctx)
{
    if (!ctx || !ctx->sse_buf || ctx->sse_buf_len == 0) return;
    for (;;) {
        size_t delim_len = 0;
        size_t pos = http_stream_find_event_delim(ctx->sse_buf, ctx->sse_buf_len, &delim_len);
        if (pos == (size_t)-1) break;

        char *event_name = NULL;
        char *event_id = NULL;
        int retry_ms = -1;
        char *data = (char *)calloc(1, pos + 1);
        size_t data_len = 0;
        if (!data) return;

        size_t start = 0;
        while (start < pos) {
            size_t end = start;
            while (end < pos && ctx->sse_buf[end] != '\n') end++;
            size_t line_len = end - start;
            if (line_len > 0 && ctx->sse_buf[start + line_len - 1] == '\r') line_len--;
            const char *line = ctx->sse_buf + start;

            if (line_len > 0 && line[0] != ':') {
                const char *colon = memchr(line, ':', line_len);
                size_t key_len = colon ? (size_t)(colon - line) : line_len;
                const char *val = colon ? colon + 1 : "";
                size_t val_len = colon ? (line_len - key_len - 1) : 0;
                if (val_len > 0 && val[0] == ' ') {
                    val++;
                    val_len--;
                }
                if (key_len == 5 && memcmp(line, "event", 5) == 0) {
                    free(event_name);
                    event_name = (char *)malloc(val_len + 1);
                    if (event_name) {
                        memcpy(event_name, val, val_len);
                        event_name[val_len] = '\0';
                    }
                } else if (key_len == 2 && memcmp(line, "id", 2) == 0) {
                    free(event_id);
                    event_id = (char *)malloc(val_len + 1);
                    if (event_id) {
                        memcpy(event_id, val, val_len);
                        event_id[val_len] = '\0';
                    }
                } else if (key_len == 5 && memcmp(line, "retry", 5) == 0) {
                    if (val_len > 0) {
                        char tmp[32];
                        size_t n = val_len >= sizeof(tmp) ? (sizeof(tmp) - 1) : val_len;
                        memcpy(tmp, val, n);
                        tmp[n] = '\0';
                        int r = atoi(tmp);
                        if (r > 0 && r < 120000) retry_ms = r;
                    }
                } else if (key_len == 4 && memcmp(line, "data", 4) == 0) {
                    if (data_len > 0) data[data_len++] = '\n';
                    if (val_len > 0) {
                        memcpy(data + data_len, val, val_len);
                        data_len += val_len;
                    }
                    data[data_len] = '\0';
                }
            }
            start = end + 1;
        }

        if (data_len > 0) {
            http_stream_post_sse_ex(ctx, event_name, data, event_id, retry_ms);
        }
        free(event_name);
        free(event_id);
        free(data);

        size_t consume = pos + delim_len;
        size_t remain = ctx->sse_buf_len - consume;
        memmove(ctx->sse_buf, ctx->sse_buf + consume, remain);
        ctx->sse_buf_len = remain;
        ctx->sse_buf[ctx->sse_buf_len] = '\0';
    }
}

static void http_stream_finish(http_stream_req_t *ctx, mimi_err_t result)
{
    if (!ctx || ctx->done) return;
    ctx->done = true;
    ctx->result = result;
    http_stream_destroy_timeout_timer(ctx);
    if (ctx->conn) ctx->conn->is_closing = 1;
}

static void http_stream_timeout_cb(void *arg)
{
    http_stream_req_t *ctx = (http_stream_req_t *)arg;
    if (!ctx || ctx->done) return;
    http_stream_finish(ctx, MIMI_ERR_TIMEOUT);
}

/* After response headers: idle limit between chunks (httpx `read`); reset on each read. */
static void http_stream_rearm_read_idle(http_stream_req_t *ctx)
{
    if (!ctx || ctx->done || !ctx->opened || !ctx->mgr) return;
    http_stream_destroy_timeout_timer(ctx);
    ctx->timeout_timer =
        mg_timer_add(ctx->mgr, (uint64_t)ctx->read_idle_ms, 0, http_stream_timeout_cb, ctx);
}

static void http_send_request_async(struct mg_connection *c, http_async_req_t *ctx)
{
    struct mg_str host = mg_url_host(ctx->req.url);
    const char *uri = mg_url_uri(ctx->req.url);
    if (!uri) uri = "/";
    int port = mg_url_port(ctx->req.url);
    bool is_ssl = mg_url_is_ssl(ctx->req.url);
    if (port == 0) port = is_ssl ? 443 : 80;

    snprintf(ctx->host_name, sizeof(ctx->host_name), "%.*s",
             (int) host.len, host.buf ? host.buf : "");
    char host_header[192];
    bool default_port = (is_ssl && port == 443) || (!is_ssl && port == 80);
    if (default_port) snprintf(host_header, sizeof(host_header), "%s", ctx->host_name);
    else snprintf(host_header, sizeof(host_header), "%s:%d", ctx->host_name, port);

    const char *extra = (ctx->req.headers && ctx->req.headers[0]) ? ctx->req.headers : "";
    size_t body_len = ctx->req.body ? ctx->req.body_len : 0;

    mg_printf(c,
              "%s %s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "Connection: keep-alive\r\n"
              "%s%s"
              "Content-Length: %lu\r\n"
              "\r\n",
              ctx->req.method ? ctx->req.method : "GET",
              uri,
              host_header,
              extra,
              (extra[0] && (extra[strlen(extra) - 1] != '\n')) ? "\r\n" : "",
              (unsigned long)body_len);

    if (body_len > 0) {
        mg_send(c, ctx->req.body, body_len);
    }
}

static void http_send_request_from_req(struct mg_connection *c,
                                       const mimi_http_request_t *req,
                                       char *host_name,
                                       size_t host_name_size)
{
    if (!c || !req || !req->url) return;
    struct mg_str host = mg_url_host(req->url);
    const char *uri = mg_url_uri(req->url);
    if (!uri) uri = "/";
    int port = mg_url_port(req->url);
    bool is_ssl = mg_url_is_ssl(req->url);
    if (port == 0) port = is_ssl ? 443 : 80;

    if (host_name && host_name_size > 0) {
        snprintf(host_name, host_name_size, "%.*s", (int)host.len, host.buf ? host.buf : "");
    }
    char host_header[192];
    bool default_port = (is_ssl && port == 443) || (!is_ssl && port == 80);
    if (default_port) snprintf(host_header, sizeof(host_header), "%.*s", (int)host.len, host.buf ? host.buf : "");
    else snprintf(host_header, sizeof(host_header), "%.*s:%d", (int)host.len, host.buf ? host.buf : "", port);

    const char *extra = (req->headers && req->headers[0]) ? req->headers : "";
    size_t body_len = req->body ? req->body_len : 0;
    mg_printf(c,
              "%s %s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "Connection: keep-alive\r\n"
              "%s%s"
              "Content-Length: %lu\r\n"
              "\r\n",
              req->method ? req->method : "GET",
              uri,
              host_header,
              extra,
              (extra[0] && (extra[strlen(extra) - 1] != '\n')) ? "\r\n" : "",
              (unsigned long)body_len);

    if (body_len > 0) mg_send(c, req->body, body_len);
}

static void http_signal_done(http_ctx_t *ctx, mimi_err_t result)
{
    if (!ctx || !ctx->mutex || !ctx->cond) return;
    mimi_mutex_lock(ctx->mutex);
    ctx->result = result;
    ctx->done = true;
    mimi_cond_signal(ctx->cond);
    mimi_mutex_unlock(ctx->mutex);
}

static void http_set_content_type(mimi_http_response_t *resp, const struct mg_http_message *hm)
{
    if (!resp || !hm) return;
    if (resp->content_type) return;

    struct mg_str *ct = mg_http_get_header((struct mg_http_message *)hm, "Content-Type");
    if (!ct || !ct->buf || ct->len <= 0) return;

    char *s = (char *)malloc((size_t)ct->len + 1);
    if (!s) return;
    memcpy(s, ct->buf, (size_t)ct->len);
    s[ct->len] = '\0';
    resp->content_type = s;
}

static bool header_name_equals(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool header_name_equals_len(const char *a, size_t a_len, const char *b)
{
    if (!a || !b) return false;
    size_t b_len = strlen(b);
    if (a_len != b_len) return false;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

static int captured_header_index_by_name(const mimi_http_response_t *resp, const char *name)
{
    if (!resp || !resp->captured_headers || !name) return -1;
    for (size_t i = 0; i < resp->captured_headers_count; i++) {
        if (resp->captured_headers[i].name &&
            header_name_equals(resp->captured_headers[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static void captured_header_set_value(mimi_http_response_t *resp, size_t idx, const char *value, size_t len)
{
    if (!resp || !resp->captured_headers || idx >= resp->captured_headers_count) return;
    if (resp->captured_headers[idx].value) return; /* keep first occurrence */
    if (!value || len == 0) return;
    char *s = (char *)malloc(len + 1);
    if (!s) return;
    memcpy(s, value, len);
    s[len] = '\0';
    resp->captured_headers[idx].value = s;
}

static void http_response_init_captured_headers(mimi_http_response_t *resp, const mimi_http_request_t *req)
{
    if (!resp) return;
    resp->captured_headers = NULL;
    resp->captured_headers_count = 0;

    if (!req || !req->capture_response_headers || req->capture_response_headers_count == 0) return;

    resp->captured_headers_count = req->capture_response_headers_count;
    resp->captured_headers = (mimi_http_header_kv_t *)calloc(resp->captured_headers_count,
                                                           sizeof(mimi_http_header_kv_t));
    if (!resp->captured_headers) {
        resp->captured_headers_count = 0;
        return;
    }

    for (size_t i = 0; i < resp->captured_headers_count; i++) {
        const char *n = req->capture_response_headers[i];
        if (n && n[0]) resp->captured_headers[i].name = strdup(n);
    }
}

static void http_capture_captured_headers_from_mg(mimi_http_response_t *resp, const struct mg_http_message *hm)
{
    if (!resp || !hm || !resp->captured_headers || resp->captured_headers_count == 0) return;

    for (size_t i = 0; i < MG_MAX_HTTP_HEADERS; i++) {
        if (!hm->headers[i].name.buf || hm->headers[i].name.len == 0) continue;
        if (!hm->headers[i].value.buf || hm->headers[i].value.len == 0) continue;

        const char *hname = hm->headers[i].name.buf;
        size_t hname_len = hm->headers[i].name.len;
        const char *hval = hm->headers[i].value.buf;
        size_t hval_len = hm->headers[i].value.len;

        for (size_t k = 0; k < resp->captured_headers_count; k++) {
            if (!resp->captured_headers[k].name) continue;
            if (header_name_equals_len(hname, hname_len, resp->captured_headers[k].name)) {
                captured_header_set_value(resp, k, hval, hval_len);
                break;
            }
        }
    }
}

static bool try_capture_sse_from_raw(struct mg_connection *c, mimi_http_response_t *resp, const mimi_http_request_t *req)
{
    (void)req; /* header capture is driven by resp->captured_headers */
    if (!c || !resp || !c->recv.buf || c->recv.len == 0 || resp->body) return false;
    const char *raw = (const char *)c->recv.buf;
    size_t raw_len = c->recv.len;

    size_t hdr_end = 0;
    for (size_t i = 0; i + 3 < raw_len; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }

    bool is_sse = false;
    const char *body = raw;
    size_t body_len = raw_len;

    int status = resp->status;

    /* Case A: buffer includes HTTP headers (status line + headers + body). */
    if (hdr_end > 0) {
        body = raw + hdr_end;
        body_len = raw_len - hdr_end;
        if (body_len == 0) return false;

        char *headers = (char *)malloc(hdr_end + 1);
        if (!headers) return false;
        memcpy(headers, raw, hdr_end);
        headers[hdr_end] = '\0';

        if (sscanf(headers, "HTTP/%*s %d", &status) != 1) {
            free(headers);
            return false;
        }

        char *line = headers;
        while (line && *line) {
            char *next = strstr(line, "\r\n");
            if (!next) break;
            *next = '\0';
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                char *name = line;
                char *value = colon + 1;
                while (*value == ' ' || *value == '\t') value++;
                if (header_name_equals(name, "Content-Type")) {
                    if (strstr(value, "text/event-stream")) is_sse = true;
                }
                int idx = captured_header_index_by_name(resp, name);
                if (idx >= 0) captured_header_set_value(resp, (size_t)idx, value, strlen(value));
            }
            line = next + 2;
        }

        bool looks_like_sse = (strstr(body, "event:") != NULL) || (strstr(body, "data:") != NULL);
        if (!is_sse && !looks_like_sse) {
            free(headers);
            return false;
        }

        bool has_complete_event = (strstr(body, "\n\n") != NULL) || (strstr(body, "\r\n\r\n") != NULL);
        if (!has_complete_event) {
            free(headers);
            return false;
        }

        uint8_t *buf = (uint8_t *)malloc(body_len + 1);
        if (!buf) {
            free(headers);
            return false;
        }
        memcpy(buf, body, body_len);
        buf[body_len] = '\0';
        resp->status = status;
        resp->body = buf;
        resp->body_len = body_len;
        if (!resp->content_type) resp->content_type = strdup("text/event-stream");
        free(headers);
        return true;
    }

    /* Case B: buffer doesn't include HTTP headers anymore (body-only). */
    is_sse = true;
    if (body_len == 0) return false;

    bool looks_like_sse = (strstr(body, "event:") != NULL) || (strstr(body, "data:") != NULL);
    if (!looks_like_sse) return false;

    bool has_complete_event = (strstr(body, "\n\n") != NULL) || (strstr(body, "\r\n\r\n") != NULL);
    if (!has_complete_event) return false;

    uint8_t *buf = (uint8_t *)malloc(body_len + 1);
    if (!buf) return false;
    memcpy(buf, body, body_len);
    buf[body_len] = '\0';
    resp->body = buf;
    resp->body_len = body_len;
    if (!resp->content_type) resp->content_type = strdup("text/event-stream");
    return true;
}

static void http_ev_direct_async(struct mg_connection *c, int ev, void *ev_data)
{
    http_async_req_t *ctx = (http_async_req_t *)c->fn_data;
    if (!ctx) return;
    ctx->conn = c;

    if (ev == MG_EV_CONNECT) {
        int status = 0;
        if (ev_data != NULL) status = *(int *) ev_data;
        if (status != 0) {
            MIMI_LOGE("http_posix", "Connect failed (status=%d) url=%s", status,
                      ctx->req.url ? ctx->req.url : "(null)");
            http_async_finish(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }
        struct mg_str host = mg_url_host(ctx->req.url);
        bool is_ssl = mg_url_is_ssl(ctx->req.url);
        if (is_ssl) {
            struct mg_tls_opts opts;
            memset(&opts, 0, sizeof(opts));
            char host_name[128];
            snprintf(host_name, sizeof(host_name), "%.*s",
                     (int) host.len, host.buf ? host.buf : "");
            opts.name = mg_str(host_name);
            opts.skip_verification = 1;
            mg_tls_init(c, &opts);
        } else {
            http_send_request_async(c, ctx);
        }
    } else if (ev == MG_EV_TLS_HS) {
        int status = 0;
        if (ev_data != NULL) status = *(int *) ev_data;
        if (status != 0) {
            MIMI_LOGE("http_posix", "TLS handshake failed (status=%d) url=%s", status,
                      ctx->req.url ? ctx->req.url : "(null)");
            http_async_finish(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }
        http_send_request_async(c, ctx);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        int st = mg_http_status(hm);
        if (ctx->resp) ctx->resp->status = st;
        http_set_content_type(ctx->resp, hm);
        http_capture_captured_headers_from_mg(ctx->resp, hm);

        bool wants_sse = (ctx->req.headers && strstr(ctx->req.headers, "text/event-stream") != NULL);
        bool is_sse_resp = (ctx->resp && ctx->resp->content_type &&
                            strstr(ctx->resp->content_type, "text/event-stream") != NULL);
        if ((is_sse_resp || wants_sse) && hm->body.len == 0) {
            /* SSE: don't finish on headers-only/empty body; wait for a complete SSE event in MG_EV_READ. */
            return;
        }

        size_t n = hm->body.len;
        uint8_t *buf = (uint8_t *)malloc(n + 1);
        if (!buf) {
            http_async_finish(ctx, MIMI_ERR_NO_MEM);
        } else {
            memcpy(buf, hm->body.buf, n);
            buf[n] = '\0';
            if (ctx->resp) {
                ctx->resp->body = buf;
                ctx->resp->body_len = n;
            } else {
                free(buf);
            }
            http_async_finish(ctx, MIMI_OK);
        }
        c->is_closing = 1;
    } else if (ev == MG_EV_ERROR) {
        const char *err = (const char *) ev_data;
        MIMI_LOGE("http_posix", "Mongoose HTTP error: %s", err ? err : "(unknown)");
        http_async_finish(ctx, MIMI_ERR_IO);
        c->is_closing = 1;
    } else if (ev == MG_EV_CLOSE) {
        /* Cancel timeout timer */
        http_async_destroy_timeout_timer(ctx);

        if (!ctx->done) {
            MIMI_LOGE("http_posix", "Connection closed before response (host=%s)",
                      ctx->host_name[0] ? ctx->host_name : "(unset)");
            ctx->result = MIMI_ERR_IO;
        }

        http_async_post_complete(ctx);
        http_async_free_owned(ctx);
        free(ctx);
    }
}

static void http_ev_proxy_async(struct mg_connection *c, int ev, void *ev_data)
{
    http_async_req_t *ctx = (http_async_req_t *)c->fn_data;
    if (!ctx) return;
    ctx->conn = c;

    if (ev == MG_EV_CONNECT) {
        int status = 0;
        if (ev_data != NULL) status = *(int *) ev_data;
        if (status != 0) {
            MIMI_LOGE("http_posix", "Proxy connect failed (status=%d)", status);
            http_async_finish(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }

        ctx->phase = HTTP_PHASE_PROXY_HANDSHAKE;

        struct mg_str host = mg_url_host(ctx->req.url);
        int port = mg_url_port(ctx->req.url);
        if (port == 0) port = mg_url_is_ssl(ctx->req.url) ? 443 : 80;

        char hostbuf[128];
        snprintf(hostbuf, sizeof(hostbuf), "%.*s",
                 (int) host.len, host.buf ? host.buf : "");

        MIMI_LOGI("http_posix", "Proxy CONNECT to %s:%d", hostbuf, port);

        mg_printf(c,
                  "CONNECT %s:%d HTTP/1.1\r\n"
                  "Host: %s:%d\r\n"
                  "Connection: keep-alive\r\n"
                  "\r\n",
                  hostbuf, port, hostbuf, port);
    } else if (ev == MG_EV_READ && ctx->phase == HTTP_PHASE_PROXY_HANDSHAKE) {
        size_t len = c->recv.len;
        char *buf = (char *) c->recv.buf;
        size_t header_end = 0;

        for (size_t i = 0; i + 3 < len; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                header_end = i + 4;
                break;
            }
        }

        if (header_end == 0) return;

        char line[128];
        size_t line_len = 0;
        for (size_t i = 0; i < len && i < sizeof(line) - 1; i++) {
            if (i + 1 < len && buf[i] == '\r' && buf[i + 1] == '\n') {
                line_len = i;
                break;
            }
        }
        if (line_len == 0 || line_len >= sizeof(line)) {
            MIMI_LOGE("http_posix", "Proxy response malformed");
            http_async_finish(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }

        memcpy(line, buf, line_len);
        line[line_len] = '\0';

        int code = 0;
        if (sscanf(line, "HTTP/%*s %d", &code) != 1 || code != 200) {
            MIMI_LOGE("http_posix", "Proxy CONNECT failed: %s", line);
            http_async_finish(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }

        MIMI_LOGI("http_posix", "Proxy CONNECT OK");

        mg_iobuf_del(&c->recv, 0, header_end);
        ctx->phase = HTTP_PHASE_HTTP_ACTIVE;

        http_send_request_async(c, ctx);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        int st = mg_http_status(hm);
        if (ctx->resp) ctx->resp->status = st;
        http_set_content_type(ctx->resp, hm);
        http_capture_captured_headers_from_mg(ctx->resp, hm);

        bool wants_sse = (ctx->req.headers && strstr(ctx->req.headers, "text/event-stream") != NULL);
        bool is_sse_resp = (ctx->resp && ctx->resp->content_type &&
                            strstr(ctx->resp->content_type, "text/event-stream") != NULL);
        if ((is_sse_resp || wants_sse) && hm->body.len == 0) {
            /* SSE: don't finish on headers-only/empty body; wait for a complete SSE event in MG_EV_READ. */
            return;
        }

        size_t n = hm->body.len;
        uint8_t *b = (uint8_t *)malloc(n + 1);
        if (!b) {
            http_async_finish(ctx, MIMI_ERR_NO_MEM);
        } else {
            memcpy(b, hm->body.buf, n);
            b[n] = '\0';
            if (ctx->resp) {
                ctx->resp->body = b;
                ctx->resp->body_len = n;
            } else {
                free(b);
            }
            http_async_finish(ctx, MIMI_OK);
        }
        c->is_closing = 1;
    } else if (ev == MG_EV_ERROR) {
        const char *err = (const char *) ev_data;
        MIMI_LOGE("http_posix", "Mongoose HTTP error (proxy): %s", err ? err : "(unknown)");
        http_async_finish(ctx, MIMI_ERR_IO);
        c->is_closing = 1;
    } else if (ev == MG_EV_CLOSE) {
        http_async_destroy_timeout_timer(ctx);

        if (!ctx->done) {
            MIMI_LOGE("http_posix", "Connection closed before response (proxy)");
            ctx->result = MIMI_ERR_IO;
        }

        http_async_post_complete(ctx);
        http_async_free_owned(ctx);
        free(ctx);
    }
}

/* Same semantics as mongoose skip_chunk() (not exported). */
static bool http_is_hex_digit(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int http_skip_chunk(const char *buf, int len, int *pl, int *dl)
{
    int i = 0, n = 0;
    if (len < 3) return 0;
    while (i < len && http_is_hex_digit(buf[i])) i++;
    if (i == 0) return -1;
    if (i > (int)sizeof(int) * 2) return -1;
    if (len < i + 1 || buf[i] != '\r' || buf[i + 1] != '\n') return -1;
    if (mg_str_to_num(mg_str_n(buf, (size_t)i), 16, &n, sizeof(n)) == false) return -1;
    if (n < 0) return -1;
    if (n > len - i - 4) return 0;
    if (buf[i + n + 2] != '\r' || buf[i + n + 3] != '\n') return -1;
    *pl = i + 2;
    *dl = n;
    return i + 2 + n + 2;
}

static void http_stream_consume_body(http_stream_req_t *ctx, struct mg_connection *c)
{
    if (!ctx || !c || ctx->done) return;

    if (ctx->body_mode == HTTP_STREAM_BODY_CHUNKED) {
        for (;;) {
            if (c->recv.len == 0) return;
            int pl = 0, dl = 0;
            int cl = http_skip_chunk((char *)c->recv.buf, (int)c->recv.len, &pl, &dl);
            if (cl == 0) return;
            if (cl < 0) {
                http_stream_finish(ctx, MIMI_ERR_IO);
                return;
            }
            if (dl == 0) {
                mg_iobuf_del(&c->recv, 0, (size_t)cl);
                http_stream_finish(ctx, MIMI_OK);
                return;
            }
            const uint8_t *payload = (uint8_t *)c->recv.buf + (size_t)pl;
            if (ctx->is_sse) {
                if (http_stream_sse_append(ctx, payload, (size_t)dl) != MIMI_OK) {
                    http_stream_finish(ctx, MIMI_ERR_NO_MEM);
                    return;
                }
                http_stream_parse_sse_events(ctx);
            } else {
                http_stream_post_data(ctx, payload, (size_t)dl);
            }
            mg_iobuf_del(&c->recv, 0, (size_t)cl);
            http_stream_rearm_read_idle(ctx);
        }
    } else if (ctx->body_mode == HTTP_STREAM_BODY_FIXED_LEN) {
        if (ctx->fixed_remain == 0) return;
        size_t take = c->recv.len;
        if (take > ctx->fixed_remain) take = ctx->fixed_remain;
        if (take == 0) return;
        if (ctx->is_sse) {
            if (http_stream_sse_append(ctx, c->recv.buf, take) != MIMI_OK) {
                http_stream_finish(ctx, MIMI_ERR_NO_MEM);
                return;
            }
            http_stream_parse_sse_events(ctx);
        } else {
            http_stream_post_data(ctx, c->recv.buf, take);
        }
        ctx->fixed_remain -= take;
        mg_iobuf_del(&c->recv, 0, take);
        http_stream_rearm_read_idle(ctx);
        if (ctx->fixed_remain == 0) http_stream_finish(ctx, MIMI_OK);
    } else {
        /* RAW / until-close */
        if (c->recv.len == 0) return;
        if (ctx->is_sse) {
            if (http_stream_sse_append(ctx, c->recv.buf, c->recv.len) != MIMI_OK) {
                http_stream_finish(ctx, MIMI_ERR_NO_MEM);
                return;
            }
            http_stream_parse_sse_events(ctx);
        } else {
            http_stream_post_data(ctx, c->recv.buf, c->recv.len);
        }
        mg_iobuf_del(&c->recv, 0, c->recv.len);
        http_stream_rearm_read_idle(ctx);
    }
}

/* Raw TCP/TLS read path (mg_connect, no mongoose http_cb). Required for infinite
 * chunked SSE: http_cb buffers until a terminal zero-length chunk. */
static void http_stream_on_read(http_stream_req_t *ctx, struct mg_connection *c)
{
    if (!ctx || !c || c->recv.len == 0) return;

    if (!ctx->headers_parsed) {
        struct mg_http_message hm;
        int n = mg_http_parse((char *)c->recv.buf, c->recv.len, &hm);
        if (n < 0) {
            http_stream_finish(ctx, MIMI_ERR_IO);
            return;
        }
        if (n == 0) return;

        ctx->open_meta.status = mg_http_status(&hm);
        http_set_content_type(&ctx->open_meta, &hm);
        http_capture_captured_headers_from_mg(&ctx->open_meta, &hm);
        ctx->is_sse = ctx->open_meta.content_type &&
                      strstr(ctx->open_meta.content_type, "text/event-stream") != NULL;
        if (!ctx->is_sse && ctx->req.headers &&
            strstr(ctx->req.headers, "text/event-stream") != NULL) {
            ctx->is_sse = true;
        }

        ctx->body_mode = (uint8_t)HTTP_STREAM_BODY_RAW;
        struct mg_str *te = mg_http_get_header(&hm, "Transfer-Encoding");
        if (te && mg_strcasecmp(*te, mg_str("chunked")) == 0) {
            ctx->body_mode = (uint8_t)HTTP_STREAM_BODY_CHUNKED;
        } else {
            struct mg_str *clh = mg_http_get_header(&hm, "Content-Length");
            if (clh && clh->buf && clh->len > 0 && clh->len < 64) {
                char tmp[64];
                memcpy(tmp, clh->buf, clh->len);
                tmp[clh->len] = '\0';
                unsigned long long clv = strtoull(tmp, NULL, 10);
                ctx->fixed_remain = (size_t)clv;
                ctx->body_mode = (uint8_t)HTTP_STREAM_BODY_FIXED_LEN;
            }
        }

        ctx->headers_parsed = true;
        http_stream_post_open(ctx);
        mg_iobuf_del(&c->recv, 0, (size_t)n);
    }

    http_stream_consume_body(ctx, c);
}

static void http_ev_stream_direct(struct mg_connection *c, int ev, void *ev_data)
{
    http_stream_req_t *ctx = (http_stream_req_t *)c->fn_data;
    if (!ctx) return;
    ctx->conn = c;

    if (ctx->handle &&
        atomic_load_explicit(&ctx->handle->close_requested, memory_order_acquire) &&
        !ctx->done) {
        http_stream_finish(ctx, MIMI_ERR_EXIT);
    }

    if (ev == MG_EV_CONNECT) {
        int status = ev_data ? *(int *)ev_data : 0;
        if (status != 0) {
            http_stream_finish(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }
        struct mg_str host = mg_url_host(ctx->req.url);
        if (mg_url_is_ssl(ctx->req.url)) {
            struct mg_tls_opts opts;
            memset(&opts, 0, sizeof(opts));
            char host_name[128];
            snprintf(host_name, sizeof(host_name), "%.*s", (int)host.len, host.buf ? host.buf : "");
            opts.name = mg_str(host_name);
            opts.skip_verification = 1;
            mg_tls_init(c, &opts);
        } else {
            http_send_request_from_req(c, &ctx->req, ctx->host_name, sizeof(ctx->host_name));
        }
    } else if (ev == MG_EV_TLS_HS) {
        int status = ev_data ? *(int *)ev_data : 0;
        if (status != 0) {
            http_stream_finish(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }
        http_send_request_from_req(c, &ctx->req, ctx->host_name, sizeof(ctx->host_name));
    } else if (ev == MG_EV_READ) {
        http_stream_on_read(ctx, c);
    } else if (ev == MG_EV_ERROR) {
        http_stream_finish(ctx, MIMI_ERR_IO);
        c->is_closing = 1;
    } else if (ev == MG_EV_CLOSE) {
        http_stream_destroy_timeout_timer(ctx);
        if (!ctx->done) ctx->result = MIMI_ERR_IO;
        http_stream_post_close(ctx);
        if (ctx->handle) {
            http_stream_handle_release(ctx->handle); /* release stream context ownership */
            ctx->handle = NULL;
        }
        http_stream_free_owned(ctx);
        free(ctx);
    }
}

static void http_ev_direct(struct mg_connection *c, int ev, void *ev_data)
{
    http_ctx_t *ctx = (http_ctx_t *)c->fn_data;
    if (!ctx) return;

    if (ev == MG_EV_CONNECT) {
        int status = 0;
        if (ev_data != NULL) status = *(int *) ev_data;
        if (status != 0) {
            MIMI_LOGE("http_posix", "Connect failed (status=%d) url=%s", status,
                      ctx->req && ctx->req->url ? ctx->req->url : "(null)");
            http_signal_done(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }

        struct mg_str host = mg_url_host(ctx->req->url);
        bool is_ssl = mg_url_is_ssl(ctx->req->url);
        if (is_ssl) {
            struct mg_tls_opts opts;
            memset(&opts, 0, sizeof(opts));
            char host_name[128];
            snprintf(host_name, sizeof(host_name), "%.*s",
                     (int) host.len, host.buf ? host.buf : "");
            opts.name = mg_str(host_name);
            opts.skip_verification = 1;
            mg_tls_init(c, &opts);
        } else {
            http_send_request(c, ctx);
        }
    } else if (ev == MG_EV_TLS_HS) {
        int status = 0;
        if (ev_data != NULL) status = *(int *) ev_data;
        if (status != 0) {
            MIMI_LOGE("http_posix", "TLS handshake failed (status=%d) url=%s", status,
                      ctx->req && ctx->req->url ? ctx->req->url : "(null)");
            http_signal_done(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }
        http_send_request(c, ctx);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        int st = mg_http_status(hm);
        ctx->resp->status = st;
        http_set_content_type(ctx->resp, hm);
        http_capture_captured_headers_from_mg(ctx->resp, hm);

        bool is_sse_resp = (ctx->resp && ctx->resp->content_type &&
                            strstr(ctx->resp->content_type, "text/event-stream") != NULL);
        if (is_sse_resp && hm->body.len == 0) {
            /* SSE: don't finish on headers-only/empty body; wait for a complete SSE event in MG_EV_READ. */
            return;
        }

        size_t n = hm->body.len;
        uint8_t *buf = (uint8_t *)malloc(n + 1);
        if (!buf) {
            http_signal_done(ctx, MIMI_ERR_NO_MEM);
        } else {
            memcpy(buf, hm->body.buf, n);
            buf[n] = '\0';
            ctx->resp->body = buf;
            ctx->resp->body_len = n;
            http_signal_done(ctx, MIMI_OK);
        }
        c->is_closing = 1;
    } else if (ev == MG_EV_ERROR) {
        const char *err = (const char *) ev_data;
        MIMI_LOGE("http_posix", "Mongoose HTTP error: %s", err ? err : "(unknown)");
        http_signal_done(ctx, MIMI_ERR_IO);
    } else if (ev == MG_EV_READ) {
        if (try_capture_sse_from_raw(c, ctx->resp, ctx->req)) {
            http_signal_done(ctx, MIMI_OK);
            c->is_closing = 1;
        }
    } else if (ev == MG_EV_CLOSE) {
        if (ctx->mutex && ctx->cond) {
            mimi_mutex_lock(ctx->mutex);
            ctx->closed = true;
            if (!ctx->done) {
                MIMI_LOGE("http_posix", "Connection closed before response (host=%s)",
                          ctx->host_name[0] ? ctx->host_name : "(unset)");
                ctx->result = MIMI_ERR_IO;
                ctx->done = true;
            }
            mimi_cond_signal(ctx->cond);
            mimi_mutex_unlock(ctx->mutex);
        }
    }
}

static void http_ev_proxy(struct mg_connection *c, int ev, void *ev_data)
{
    http_ctx_t *ctx = (http_ctx_t *)c->fn_data;
    if (!ctx) return;

    if (ev == MG_EV_CONNECT) {
        int status = 0;
        if (ev_data != NULL) status = *(int *) ev_data;
        if (status != 0) {
            MIMI_LOGE("http_posix", "Proxy connect failed (status=%d)", status);
            http_signal_done(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }

        ctx->phase = HTTP_PHASE_PROXY_HANDSHAKE;

        struct mg_str host = mg_url_host(ctx->req->url);
        int port = mg_url_port(ctx->req->url);
        if (port == 0) {
            port = mg_url_is_ssl(ctx->req->url) ? 443 : 80;
        }

        char hostbuf[128];
        snprintf(hostbuf, sizeof(hostbuf), "%.*s",
                 (int) host.len, host.buf ? host.buf : "");

        MIMI_LOGI("http_posix", "Proxy CONNECT to %s:%d", hostbuf, port);

        mg_printf(c,
                  "CONNECT %s:%d HTTP/1.1\r\n"
                  "Host: %s:%d\r\n"
                  "Connection: keep-alive\r\n"
                  "\r\n",
                  hostbuf, port, hostbuf, port);
    } else if (ev == MG_EV_READ && ctx->phase == HTTP_PHASE_PROXY_HANDSHAKE) {
        size_t len = c->recv.len;
        char *buf = (char *) c->recv.buf;
        size_t header_end = 0;

        for (size_t i = 0; i + 3 < len; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                header_end = i + 4;
                break;
            }
        }

        if (header_end == 0) {
            return;
        }

        char line[128];
        size_t line_len = 0;
        for (size_t i = 0; i < len && i < sizeof(line) - 1; i++) {
            if (i + 1 < len && buf[i] == '\r' && buf[i + 1] == '\n') {
                line_len = i;
                break;
            }
        }
        if (line_len == 0 || line_len >= sizeof(line)) {
            MIMI_LOGE("http_posix", "Proxy response malformed");
            http_signal_done(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }

        memcpy(line, buf, line_len);
        line[line_len] = '\0';

        int code = 0;
        if (sscanf(line, "HTTP/%*s %d", &code) != 1 || code != 200) {
            MIMI_LOGE("http_posix", "Proxy CONNECT failed: %s", line);
            http_signal_done(ctx, MIMI_ERR_IO);
            c->is_closing = 1;
            return;
        }

        MIMI_LOGI("http_posix", "Proxy CONNECT OK");

        mg_iobuf_del(&c->recv, 0, header_end);

        ctx->phase = HTTP_PHASE_HTTP_ACTIVE;

        http_send_request(c, ctx);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        int st = mg_http_status(hm);
        ctx->resp->status = st;
        http_set_content_type(ctx->resp, hm);
        http_capture_captured_headers_from_mg(ctx->resp, hm);

        bool wants_sse = (ctx->req->headers && strstr(ctx->req->headers, "text/event-stream") != NULL);
        bool is_sse_resp = (ctx->resp && ctx->resp->content_type &&
                            strstr(ctx->resp->content_type, "text/event-stream") != NULL);
        if ((is_sse_resp || wants_sse) && hm->body.len == 0) {
            /* SSE: don't finish on headers-only/empty body; wait for a complete SSE event in MG_EV_READ. */
            return;
        }

        size_t n = hm->body.len;
        uint8_t *buf = (uint8_t *)malloc(n + 1);
        if (!buf) {
            http_signal_done(ctx, MIMI_ERR_NO_MEM);
        } else {
            memcpy(buf, hm->body.buf, n);
            buf[n] = '\0';
            ctx->resp->body = buf;
            ctx->resp->body_len = n;
            http_signal_done(ctx, MIMI_OK);
        }
        c->is_closing = 1;
    } else if (ev == MG_EV_ERROR) {
        const char *err = (const char *) ev_data;
        MIMI_LOGE("http_posix", "Mongoose HTTP error (proxy): %s", err ? err : "(unknown)");
        http_signal_done(ctx, MIMI_ERR_IO);
    } else if (ev == MG_EV_READ) {
        if (ctx->phase == HTTP_PHASE_HTTP_ACTIVE && try_capture_sse_from_raw(c, ctx->resp, ctx->req)) {
            http_signal_done(ctx, MIMI_OK);
            c->is_closing = 1;
        }
    } else if (ev == MG_EV_CLOSE) {
        if (ctx->mutex && ctx->cond) {
            mimi_mutex_lock(ctx->mutex);
            ctx->closed = true;
            if (!ctx->done) {
                MIMI_LOGE("http_posix", "Connection closed before response (proxy)");
                ctx->result = MIMI_ERR_IO;
                ctx->done = true;
            }
            mimi_cond_signal(ctx->cond);
            mimi_mutex_unlock(ctx->mutex);
        }
    }
}

static mimi_err_t mimi_http_exec_direct(const mimi_http_request_t *req, mimi_http_response_t *resp)
{
    http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.req = req;
    ctx.resp = resp;
    ctx.result = MIMI_ERR_TIMEOUT;
    ctx.use_proxy = false;
    ctx.phase = HTTP_PHASE_DIRECT;

    if (mimi_mutex_create(&ctx.mutex) != MIMI_OK) {
        return MIMI_ERR_FAIL;
    }
    if (mimi_cond_create(&ctx.cond) != MIMI_OK) {
        mimi_mutex_destroy(ctx.mutex);
        return MIMI_ERR_FAIL;
    }

    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!mgr) {
        mimi_cond_destroy(ctx.cond);
        mimi_mutex_destroy(ctx.mutex);
        MIMI_LOGE("http_posix", "Runtime event loop not available");
        return MIMI_ERR_INVALID_STATE;
    }

    struct mg_connection *c = mg_http_connect(mgr, req->url, http_ev_direct, &ctx);
    if (!c) {
        mimi_cond_destroy(ctx.cond);
        mimi_mutex_destroy(ctx.mutex);
        return MIMI_ERR_IO;
    }

    uint64_t start = mg_millis();
    uint32_t timeout = req->timeout_ms ? req->timeout_ms : 30000;

    mimi_mutex_lock(ctx.mutex);
    while (!ctx.done) {
        if ((mg_millis() - start) > timeout) {
            ctx.result = MIMI_ERR_TIMEOUT;
            break;
        }
        mimi_cond_wait(ctx.cond, ctx.mutex, 100);
    }

    /* Ensure MG_EV_CLOSE is observed before we destroy sync primitives */
    if (!ctx.closed) {
        c->is_closing = 1;
        uint64_t close_start = mg_millis();
        while (!ctx.closed && (mg_millis() - close_start) < 2000) {
            mimi_cond_wait(ctx.cond, ctx.mutex, 100);
        }
    }
    mimi_mutex_unlock(ctx.mutex);

    /* Mark mutex/cond as invalid before destroying */
    mimi_mutex_t *mutex = ctx.mutex;
    mimi_cond_t *cond = ctx.cond;
    ctx.mutex = NULL;
    ctx.cond = NULL;

    mimi_cond_destroy(cond);
    mimi_mutex_destroy(mutex);

    if (!ctx.done && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
    return ctx.result;
}

static mimi_err_t mimi_http_exec_via_proxy(const mimi_http_request_t *req, mimi_http_response_t *resp)
{
    http_proxy_config_t cfg;
    mimi_err_t perr = http_proxy_get_config(&cfg);
    if (perr != MIMI_OK) {
        return perr;
    }
    if (strcmp(cfg.type, "http") != 0) {
        MIMI_LOGE("http_posix", "Proxy type '%s' not supported on POSIX (only 'http')", cfg.type);
        return MIMI_ERR_NOT_SUPPORTED;
    }

    http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.req = req;
    ctx.resp = resp;
    ctx.result = MIMI_ERR_TIMEOUT;
    ctx.use_proxy = true;
    ctx.phase = HTTP_PHASE_PROXY_HANDSHAKE;

    if (mimi_mutex_create(&ctx.mutex) != MIMI_OK) {
        return MIMI_ERR_FAIL;
    }
    if (mimi_cond_create(&ctx.cond) != MIMI_OK) {
        mimi_mutex_destroy(ctx.mutex);
        return MIMI_ERR_FAIL;
    }

    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!mgr) {
        mimi_cond_destroy(ctx.cond);
        mimi_mutex_destroy(ctx.mutex);
        MIMI_LOGE("http_posix", "Runtime event loop not available");
        return MIMI_ERR_INVALID_STATE;
    }

    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://%s:%u", cfg.host, (unsigned)cfg.port);

    MIMI_LOGI("http_posix", "Connecting via HTTP proxy %s", proxy_url);

    struct mg_connection *c = mg_http_connect(mgr, proxy_url, http_ev_proxy, &ctx);
    if (!c) {
        mimi_cond_destroy(ctx.cond);
        mimi_mutex_destroy(ctx.mutex);
        return MIMI_ERR_IO;
    }

    uint64_t start = mg_millis();
    uint32_t timeout = req->timeout_ms ? req->timeout_ms : 30000;

    mimi_mutex_lock(ctx.mutex);
    while (!ctx.done) {
        if ((mg_millis() - start) > timeout) {
            ctx.result = MIMI_ERR_TIMEOUT;
            break;
        }
        mimi_cond_wait(ctx.cond, ctx.mutex, 100);
    }

    /* Ensure MG_EV_CLOSE is observed before we destroy sync primitives */
    if (!ctx.closed) {
        c->is_closing = 1;
        uint64_t close_start = mg_millis();
        while (!ctx.closed && (mg_millis() - close_start) < 2000) {
            mimi_cond_wait(ctx.cond, ctx.mutex, 100);
        }
    }
    mimi_mutex_unlock(ctx.mutex);

    /* Mark mutex/cond as invalid before destroying */
    mimi_mutex_t *mutex = ctx.mutex;
    mimi_cond_t *cond = ctx.cond;
    ctx.mutex = NULL;
    ctx.cond = NULL;

    mimi_cond_destroy(cond);
    mimi_mutex_destroy(mutex);

    if (!ctx.done && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
    return ctx.result;
}

mimi_err_t mimi_http_exec(const mimi_http_request_t *req, mimi_http_response_t *resp)
{
    if (!req || !resp || !req->url || !req->url[0]) return MIMI_ERR_INVALID_ARG;

    memset(resp, 0, sizeof(*resp));
    http_response_init_captured_headers(resp, req);

    if (http_proxy_is_enabled()) {
        return mimi_http_exec_via_proxy(req, resp);
    }

    return mimi_http_exec_direct(req, resp);
}

mimi_err_t mimi_http_exec_async(const mimi_http_request_t *req, mimi_http_response_t *resp,
                                       mimi_http_callback_t callback, void *user_data)
{
    if (!req) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (req->mode == MIMI_HTTP_CALL_STREAM) {
        if (!req->stream_callbacks || !req->out_stream_handle) return MIMI_ERR_INVALID_ARG;
        return http_stream_open_internal(req, req->stream_callbacks, user_data, req->out_stream_handle);
    }

    if (!resp || !callback) return MIMI_ERR_INVALID_ARG;

    event_bus_t *bus = event_bus_get_global();
    if (!bus) return MIMI_ERR_INVALID_STATE;

    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!mgr) return MIMI_ERR_INVALID_STATE;

    memset(resp, 0, sizeof(*resp));
    http_response_init_captured_headers(resp, req);

    http_async_req_t *ctx = (http_async_req_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return MIMI_ERR_NO_MEM;

    ctx->req.method = req->method ? strdup(req->method) : NULL;
    ctx->req.url = req->url ? strdup(req->url) : NULL;
    ctx->req.headers = req->headers ? strdup(req->headers) : NULL;
    ctx->req.timeout_ms = req->timeout_ms;
    ctx->req.capture_response_headers_count = req->capture_response_headers_count;
    if (req->capture_response_headers && req->capture_response_headers_count > 0) {
        ctx->req.capture_response_headers = (const char **)calloc(
            req->capture_response_headers_count, sizeof(char *));
        if (!ctx->req.capture_response_headers) {
            http_async_free_owned(ctx);
            free(ctx);
            return MIMI_ERR_NO_MEM;
        }
        for (size_t i = 0; i < req->capture_response_headers_count; i++) {
            const char *n = req->capture_response_headers[i];
            if (n && n[0]) ((char **)ctx->req.capture_response_headers)[i] = strdup(n);
        }
    } else {
        ctx->req.capture_response_headers = NULL;
        ctx->req.capture_response_headers_count = 0;
    }
    ctx->req.body = NULL;
    ctx->req.body_len = 0;
    if (req->body && req->body_len > 0) {
        uint8_t *b = (uint8_t *)malloc(req->body_len);
        if (!b) {
            http_async_free_owned(ctx);
            free(ctx);
            return MIMI_ERR_NO_MEM;
        }
        memcpy(b, req->body, req->body_len);
        ctx->req.body = b;
        ctx->req.body_len = req->body_len;
    }

    if (!ctx->req.url || !ctx->req.url[0]) {
        http_async_free_owned(ctx);
        free(ctx);
        return MIMI_ERR_INVALID_ARG;
    }

    ctx->resp = resp;
    ctx->callback = callback;
    ctx->callback_data = user_data;
    ctx->result = MIMI_ERR_TIMEOUT;
    ctx->done = false;
    ctx->posted = false;
    ctx->bus = bus;
    ctx->mgr = mgr;

    uint32_t timeout = ctx->req.timeout_ms ? ctx->req.timeout_ms : 30000;
    ctx->timeout_timer = mg_timer_add(mgr, (uint64_t)timeout, 0, http_async_timeout_cb, ctx);

    if (http_proxy_is_enabled()) {
        http_proxy_config_t cfg;
        mimi_err_t perr = http_proxy_get_config(&cfg);
        if (perr != MIMI_OK) {
            http_async_destroy_timeout_timer(ctx);
            http_async_free_owned(ctx);
            free(ctx);
            return perr;
        }
        if (strcmp(cfg.type, "http") != 0) {
            http_async_destroy_timeout_timer(ctx);
            http_async_free_owned(ctx);
            free(ctx);
            return MIMI_ERR_NOT_SUPPORTED;
        }

        char proxy_url[128];
        snprintf(proxy_url, sizeof(proxy_url), "http://%s:%u", cfg.host, (unsigned)cfg.port);
        ctx->phase = HTTP_PHASE_PROXY_HANDSHAKE;
        ctx->conn = mg_http_connect(mgr, proxy_url, http_ev_proxy_async, ctx);
    } else {
        ctx->phase = HTTP_PHASE_DIRECT;
        ctx->conn = mg_http_connect(mgr, ctx->req.url, http_ev_direct_async, ctx);
    }

    if (!ctx->conn) {
        http_async_destroy_timeout_timer(ctx);
        http_async_free_owned(ctx);
        free(ctx);
        return MIMI_ERR_IO;
    }

    return MIMI_OK;
}

static mimi_err_t http_stream_open_internal(const mimi_http_request_t *req,
                                            const mimi_http_stream_callbacks_t *callbacks,
                                            void *user_data,
                                            mimi_http_stream_handle_t **out_handle)
{
    if (!req || !callbacks || !out_handle || !req->url || !req->url[0]) {
        return MIMI_ERR_INVALID_ARG;
    }
    if (!callbacks->on_open && !callbacks->on_sse_event &&
        !callbacks->on_data && !callbacks->on_close) {
        return MIMI_ERR_INVALID_ARG;
    }
    if (http_proxy_is_enabled()) {
        /* Stream mode currently supports direct connections only. */
        return MIMI_ERR_NOT_SUPPORTED;
    }

    event_bus_t *bus = event_bus_get_global();
    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!bus || !mgr) return MIMI_ERR_INVALID_STATE;

    http_stream_req_t *ctx = (http_stream_req_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return MIMI_ERR_NO_MEM;
    ctx->req.method = req->method ? strdup(req->method) : NULL;
    ctx->req.url = req->url ? strdup(req->url) : NULL;
    ctx->req.headers = req->headers ? strdup(req->headers) : NULL;
    ctx->req.timeout_ms = req->timeout_ms;
    ctx->req.stream_read_idle_ms = req->stream_read_idle_ms;
    if (req->body && req->body_len > 0) {
        uint8_t *b = (uint8_t *)malloc(req->body_len);
        if (!b) {
            http_stream_free_owned(ctx);
            free(ctx);
            return MIMI_ERR_NO_MEM;
        }
        memcpy(b, req->body, req->body_len);
        ctx->req.body = b;
        ctx->req.body_len = req->body_len;
    }
    ctx->req.capture_response_headers_count = req->capture_response_headers_count;
    if (req->capture_response_headers_count > 0 && req->capture_response_headers) {
        ctx->req.capture_response_headers = (const char **)calloc(
            req->capture_response_headers_count, sizeof(char *));
        if (!ctx->req.capture_response_headers) {
            http_stream_free_owned(ctx);
            free(ctx);
            return MIMI_ERR_NO_MEM;
        }
        for (size_t i = 0; i < req->capture_response_headers_count; i++) {
            const char *n = req->capture_response_headers[i];
            if (n && n[0]) ((char **)ctx->req.capture_response_headers)[i] = strdup(n);
        }
    }
    http_response_init_captured_headers(&ctx->open_meta, &ctx->req);
    ctx->callbacks = *callbacks;
    ctx->user_data = user_data;
    ctx->bus = bus;
    ctx->mgr = mgr;
    ctx->result = MIMI_ERR_TIMEOUT;
    ctx->connect_timeout_ms = req->timeout_ms ? req->timeout_ms : 30000;
    ctx->read_idle_ms = req->stream_read_idle_ms ? req->stream_read_idle_ms : 60000;

    mimi_http_stream_handle_t *handle = (mimi_http_stream_handle_t *)calloc(1, sizeof(*handle));
    if (!handle) {
        http_stream_free_owned(ctx);
        free(ctx);
        return MIMI_ERR_NO_MEM;
    }
    atomic_init(&handle->close_requested, false);
    /* shared by stream context + caller */
    atomic_init(&handle->refcnt, 2);
    ctx->handle = handle;

    /* Until response headers: connect / time-to-first-byte (httpx pool/connect style).
     * After `http_stream_post_open`, timer switches to read-idle between chunks. */
    ctx->timeout_timer =
        mg_timer_add(mgr, (uint64_t)ctx->connect_timeout_ms, 0, http_stream_timeout_cb, ctx);
    /* mg_http_connect installs http_cb, which waits for a complete chunked body (terminal
     * zero chunk). Infinite SSE never sends that — use raw mg_connect per mongoose
     * streaming-client tutorial. */
    ctx->conn = mg_connect(mgr, ctx->req.url, http_ev_stream_direct, ctx);
    if (!ctx->conn) {
        http_stream_destroy_timeout_timer(ctx);
        free(handle);
        ctx->handle = NULL;
        http_stream_free_owned(ctx);
        free(ctx);
        return MIMI_ERR_IO;
    }

    *out_handle = handle;
    return MIMI_OK;
}

void mimi_http_exec_async_cancel(mimi_http_stream_handle_t *stream_handle)
{
    if (!stream_handle) return;
    atomic_store_explicit(&stream_handle->close_requested, true, memory_order_release);
}

void mimi_http_stream_handle_release(mimi_http_stream_handle_t *stream_handle)
{
    http_stream_handle_release(stream_handle);
}

/* HTTP event handler for internal async completions (runs on dispatcher workers) */
static void http_event_handler(event_dispatcher_t *disp, event_msg_t *msg, void *user_data)
{
    (void)disp;
    (void)user_data;

    if (msg->type == EVENT_RECV && (msg->flags & EVENT_FLAG_INTERNAL) && msg->user_data) {
        http_internal_msg_base_t *base = (http_internal_msg_base_t *)(uintptr_t)msg->user_data;
        http_internal_msg_type_t type = base->type;
        if (type == HTTP_INTERNAL_ASYNC_COMPLETE) {
            http_internal_async_complete_t *c =
                (http_internal_async_complete_t *)(uintptr_t)msg->user_data;
            if (c->callback) c->callback(c->result, c->resp, c->callback_data);
            free(c);
        } else if (type == HTTP_INTERNAL_STREAM_EVENT) {
            http_stream_event_msg_t *evt = (http_stream_event_msg_t *)(uintptr_t)msg->user_data;
            if (evt->kind == HTTP_STREAM_EVENT_OPEN && evt->callbacks.on_open) {
                evt->callbacks.on_open(&evt->open_meta, evt->user_data);
            } else if (evt->kind == HTTP_STREAM_EVENT_SSE &&
                       (evt->callbacks.on_sse_event_ex || evt->callbacks.on_sse_event)) {
                if (evt->callbacks.on_sse_event_ex) {
                    evt->callbacks.on_sse_event_ex(evt->event_name,
                                                   evt->event_data,
                                                   evt->event_id,
                                                   evt->retry_ms,
                                                   evt->user_data);
                } else if (evt->callbacks.on_sse_event) {
                    evt->callbacks.on_sse_event(evt->event_name, evt->event_data, evt->user_data);
                }
            } else if (evt->kind == HTTP_STREAM_EVENT_DATA && evt->callbacks.on_data) {
                evt->callbacks.on_data(evt->data, evt->data_len, evt->user_data);
            } else if (evt->kind == HTTP_STREAM_EVENT_CLOSE && evt->callbacks.on_close) {
                evt->callbacks.on_close(evt->err, evt->user_data);
            }
            http_stream_free_open_meta(&evt->open_meta);
            free(evt->event_name);
            free(evt->event_data);
            free(evt->event_id);
            free(evt->data);
            free(evt);
        }
    } else if (msg->type == EVENT_ERROR) {
        MIMI_LOGE("http_posix", "HTTP error: %d", msg->error_code);
    }

    if (msg->buf) {
        io_buf_unref(msg->buf);
    }
}

/* Initialize HTTP module */
mimi_err_t mimi_http_init(void)
{
    // Register HTTP event handler for async requests
    event_dispatcher_t *disp = event_dispatcher_get_global();
    if (disp) {
        event_dispatcher_register_handler(disp, CONN_HTTP_CLIENT, http_event_handler, NULL);
    }
    
    return MIMI_OK;
}

/* Deinitialize HTTP module */
void mimi_http_deinit(void)
{
    // No specific deinitialization needed
}

void mimi_http_response_free(mimi_http_response_t *resp)
{
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
    resp->status = 0;
    free(resp->content_type);
    resp->content_type = NULL;
    if (resp->captured_headers) {
        for (size_t i = 0; i < resp->captured_headers_count; i++) {
            free(resp->captured_headers[i].name);
            resp->captured_headers[i].name = NULL;
            free(resp->captured_headers[i].value);
            resp->captured_headers[i].value = NULL;
        }
        free(resp->captured_headers);
        resp->captured_headers = NULL;
    }
    resp->captured_headers_count = 0;
}

const char *mimi_http_get_captured_header_value(const mimi_http_response_t *resp, const char *name)
{
    if (!resp || !name || !resp->captured_headers) return NULL;
    for (size_t i = 0; i < resp->captured_headers_count; i++) {
        if (!resp->captured_headers[i].name) continue;
        if (header_name_equals(resp->captured_headers[i].name, name)) {
            return resp->captured_headers[i].value;
        }
    }
    return NULL;
}
