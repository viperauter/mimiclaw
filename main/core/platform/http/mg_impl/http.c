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

typedef struct {
    mimi_err_t result;
    mimi_http_response_t *resp;
    mimi_http_callback_t callback;
    void *callback_data;
} http_async_complete_t;

static void http_send_request(struct mg_connection *c, http_ctx_t *ctx)
{
    struct mg_str host = mg_url_host(ctx->req->url);
    const char *uri = mg_url_uri(ctx->req->url);
    if (!uri) uri = "/";

    snprintf(ctx->host_name, sizeof(ctx->host_name), "%.*s",
             (int) host.len, host.buf ? host.buf : "");

    if (mg_url_is_ssl(ctx->req->url)) {
        struct mg_tls_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.name = mg_str(ctx->host_name);
        opts.skip_verification = 1;
        mg_tls_init(c, &opts);
    }

    const char *extra = (ctx->req->headers && ctx->req->headers[0]) ? ctx->req->headers : "";
    size_t body_len = ctx->req->body ? ctx->req->body_len : 0;

    mg_printf(c,
              "%s %s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "Connection: close\r\n"
              "%s%s"
              "Content-Length: %lu\r\n"
              "\r\n",
              ctx->req->method ? ctx->req->method : "GET",
              uri,
              ctx->host_name,
              extra,
              (extra[0] && (extra[strlen(extra) - 1] != '\n')) ? "\r\n" : "",
              (unsigned long)body_len);

    if (body_len > 0) {
        mg_send(c, ctx->req->body, body_len);
    }
}

static void http_async_free_owned(http_async_req_t *ctx)
{
    if (!ctx) return;
    free((void *)ctx->req.method);
    free((void *)ctx->req.url);
    free((void *)ctx->req.headers);
    free((void *)ctx->req.body);
    ctx->req.method = NULL;
    ctx->req.url = NULL;
    ctx->req.headers = NULL;
    ctx->req.body = NULL;
    ctx->req.body_len = 0;
}

static void http_async_post_complete(http_async_req_t *ctx)
{
    if (!ctx || ctx->posted) return;
    ctx->posted = true;

    if (!ctx->callback) return;

    event_bus_t *bus = event_bus_get_global();
    if (!bus) return;

    http_async_complete_t *c = (http_async_complete_t *)calloc(1, sizeof(*c));
    if (!c) return;
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
    if (ctx->mgr && ctx->timeout_timer) {
        mg_timer_free(&ctx->mgr->timers, ctx->timeout_timer);
        free(ctx->timeout_timer);
        ctx->timeout_timer = NULL;
    }
    if (ctx->conn) ctx->conn->is_closing = 1;
}

static void http_async_timeout_cb(void *arg)
{
    http_async_req_t *ctx = (http_async_req_t *)arg;
    if (!ctx || ctx->done) return;
    http_async_finish(ctx, MIMI_ERR_TIMEOUT);
}

static void http_send_request_async(struct mg_connection *c, http_async_req_t *ctx)
{
    struct mg_str host = mg_url_host(ctx->req.url);
    const char *uri = mg_url_uri(ctx->req.url);
    if (!uri) uri = "/";

    snprintf(ctx->host_name, sizeof(ctx->host_name), "%.*s",
             (int) host.len, host.buf ? host.buf : "");

    if (mg_url_is_ssl(ctx->req.url)) {
        struct mg_tls_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.name = mg_str(ctx->host_name);
        opts.skip_verification = 1;
        mg_tls_init(c, &opts);
    }

    const char *extra = (ctx->req.headers && ctx->req.headers[0]) ? ctx->req.headers : "";
    size_t body_len = ctx->req.body ? ctx->req.body_len : 0;

    mg_printf(c,
              "%s %s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "Connection: close\r\n"
              "%s%s"
              "Content-Length: %lu\r\n"
              "\r\n",
              ctx->req.method ? ctx->req.method : "GET",
              uri,
              ctx->host_name,
              extra,
              (extra[0] && (extra[strlen(extra) - 1] != '\n')) ? "\r\n" : "",
              (unsigned long)body_len);

    if (body_len > 0) {
        mg_send(c, ctx->req.body, body_len);
    }
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
        http_send_request_async(c, ctx);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        int st = mg_http_status(hm);
        if (ctx->resp) ctx->resp->status = st;
        http_set_content_type(ctx->resp, hm);

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
        if (ctx->mgr && ctx->timeout_timer) {
            mg_timer_free(&ctx->mgr->timers, ctx->timeout_timer);
            free(ctx->timeout_timer);
            ctx->timeout_timer = NULL;
        }

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
        if (ctx->mgr && ctx->timeout_timer) {
            mg_timer_free(&ctx->mgr->timers, ctx->timeout_timer);
            free(ctx->timeout_timer);
            ctx->timeout_timer = NULL;
        }

        if (!ctx->done) {
            MIMI_LOGE("http_posix", "Connection closed before response (proxy)");
            ctx->result = MIMI_ERR_IO;
        }

        http_async_post_complete(ctx);
        http_async_free_owned(ctx);
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

        http_send_request(c, ctx);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        int st = mg_http_status(hm);
        ctx->resp->status = st;
        http_set_content_type(ctx->resp, hm);

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

    if (http_proxy_is_enabled()) {
        return mimi_http_exec_via_proxy(req, resp);
    }

    return mimi_http_exec_direct(req, resp);
}

mimi_err_t mimi_http_exec_async(const mimi_http_request_t *req, mimi_http_response_t *resp,
                                       mimi_http_callback_t callback, void *user_data)
{
    if (!req || !resp || !callback) {
        return MIMI_ERR_INVALID_ARG;
    }

    event_bus_t *bus = event_bus_get_global();
    if (!bus) return MIMI_ERR_INVALID_STATE;

    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!mgr) return MIMI_ERR_INVALID_STATE;

    memset(resp, 0, sizeof(*resp));

    http_async_req_t *ctx = (http_async_req_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return MIMI_ERR_NO_MEM;

    ctx->req.method = req->method ? strdup(req->method) : NULL;
    ctx->req.url = req->url ? strdup(req->url) : NULL;
    ctx->req.headers = req->headers ? strdup(req->headers) : NULL;
    ctx->req.timeout_ms = req->timeout_ms;
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
            if (ctx->timeout_timer) {
                mg_timer_free(&mgr->timers, ctx->timeout_timer);
                free(ctx->timeout_timer);
                ctx->timeout_timer = NULL;
            }
            http_async_free_owned(ctx);
            free(ctx);
            return perr;
        }
        if (strcmp(cfg.type, "http") != 0) {
            if (ctx->timeout_timer) {
                mg_timer_free(&mgr->timers, ctx->timeout_timer);
                free(ctx->timeout_timer);
                ctx->timeout_timer = NULL;
            }
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
        if (ctx->timeout_timer) {
            mg_timer_free(&mgr->timers, ctx->timeout_timer);
            free(ctx->timeout_timer);
            ctx->timeout_timer = NULL;
        }
        http_async_free_owned(ctx);
        free(ctx);
        return MIMI_ERR_IO;
    }

    return MIMI_OK;
}

/* HTTP event handler for internal async completions (runs on dispatcher workers) */
static void http_event_handler(event_dispatcher_t *disp, event_msg_t *msg, void *user_data)
{
    (void)disp;
    (void)user_data;

    if (msg->type == EVENT_RECV && (msg->flags & EVENT_FLAG_INTERNAL) && msg->user_data) {
        http_async_complete_t *c = (http_async_complete_t *)(uintptr_t)msg->user_data;
        if (c->callback) {
            c->callback(c->result, c->resp, c->callback_data);
        }
        free(c);
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
}
