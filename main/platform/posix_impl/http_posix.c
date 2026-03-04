#include "platform/http.h"
#include "platform/log.h"

#include "mongoose.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <netdb.h>
#include <arpa/inet.h>

typedef struct {
    const mimi_http_request_t *req;
    mimi_http_response_t *resp;
    mimi_err_t result;
    bool done;
    /* Mongoose URL host is mg_str (not NUL-terminated). Keep a persistent copy for TLS SNI. */
    char host_name[128];
} http_ctx_t;

/* Resolve hostname from URL using system resolver, return numeric IPv4 string. */
static bool resolve_host_ip(const char *url, char *ip_out, size_t ip_out_len)
{
    if (!url || !ip_out || ip_out_len == 0) return false;

    struct mg_str host = mg_url_host(url);
    if (host.len == 0 || !host.buf) return false;

    char hostbuf[128];
    if (host.len >= sizeof(hostbuf)) return false;
    memcpy(hostbuf, host.buf, host.len);
    hostbuf[host.len] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(hostbuf, NULL, &hints, &res);
    if (rc != 0 || !res) {
        return false;
    }

    bool ok = false;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
            const char *s = inet_ntop(AF_INET, &sa->sin_addr, ip_out, (socklen_t)ip_out_len);
            if (s) ok = true;
            break;
        }
    }

    freeaddrinfo(res);
    return ok;
}

static void http_ev(struct mg_connection *c, int ev, void *ev_data)
{
    http_ctx_t *ctx = (http_ctx_t *)c->fn_data;
    if (!ctx) return;

    if (ev == MG_EV_CONNECT) {
        /* Mongoose passes status as int* in ev_data, but it may be NULL in some environments.
           NULL means "unknown" here; treat it as success and rely on MG_EV_ERROR for failures. */
        int status = 0;
        if (ev_data != NULL) status = *(int *) ev_data;
        if (status != 0) {
            MIMI_LOGE("http_posix", "Connect failed (status=%d) url=%s", status,
                      ctx->req && ctx->req->url ? ctx->req->url : "(null)");
            ctx->result = MIMI_ERR_IO;
            ctx->done = true;
            c->is_closing = 1;
            return;
        }

        struct mg_str host = mg_url_host(ctx->req->url);
        const char *uri = mg_url_uri(ctx->req->url);
        if (!uri) uri = "/";

        /* Copy host into a NUL-terminated buffer for TLS/SNI and logging. */
        snprintf(ctx->host_name, sizeof(ctx->host_name), "%.*s", (int) host.len, host.buf ? host.buf : "");

        /* Enable TLS when URL is https:// (requires MG_TLS != NONE at build time) */
        if (mg_url_is_ssl(ctx->req->url)) {
            struct mg_tls_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.name = mg_str(ctx->host_name);  // enable hostname verification / SNI
            opts.skip_verification = 1;          // POSIX dev: skip CA verification
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
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        int st = mg_http_status(hm);
        ctx->resp->status = st;

        size_t n = hm->body.len;
        uint8_t *buf = (uint8_t *)malloc(n + 1);
        if (!buf) {
            ctx->result = MIMI_ERR_NO_MEM;
        } else {
            memcpy(buf, hm->body.buf, n);
            buf[n] = '\0';
            ctx->resp->body = buf;
            ctx->resp->body_len = n;
            ctx->result = MIMI_OK;
        }
        ctx->done = true;
        c->is_closing = 1;
    } else if (ev == MG_EV_ERROR) {
        const char *err = (const char *) ev_data;
        MIMI_LOGE("http_posix", "Mongoose HTTP error: %s", err ? err : "(unknown)");
        ctx->result = MIMI_ERR_IO;
        ctx->done = true;
    } else if (ev == MG_EV_CLOSE) {
        /* If connection closes before we got a response, treat as IO error */
        if (!ctx->done) {
            MIMI_LOGE("http_posix", "Connection closed before response (host=%s)", ctx->host_name[0] ? ctx->host_name : "(unset)");
            ctx->result = MIMI_ERR_IO;
            ctx->done = true;
        }
    }
}

mimi_err_t mimi_http_exec(const mimi_http_request_t *req, mimi_http_response_t *resp)
{
    if (!req || !resp || !req->url || !req->url[0]) return MIMI_ERR_INVALID_ARG;

    memset(resp, 0, sizeof(*resp));

    http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.req = req;
    ctx.resp = resp;
    ctx.result = MIMI_ERR_TIMEOUT;

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    /* Work around environments where UDP DNS (e.g. 8.8.8.8) is blocked:
       resolve host via system getaddrinfo(), then connect to numeric IP.
       This bypasses Mongoose's internal DNS, but we still send correct
       Host header and SNI using the original hostname. */
    char ip[64];
    char ip_url[512];
    const char *connect_url = req->url;

    if (resolve_host_ip(req->url, ip, sizeof(ip))) {
        const char *uri = mg_url_uri(req->url);
        if (!uri) uri = "/";

        /* Preserve original scheme prefix (e.g. "https://") by copying up to host. */
        const char *host_start = strstr(req->url, "://");
        size_t scheme_len = 0;
        if (host_start) {
            scheme_len = (host_start - req->url) + 3;  // include ://
        }

        if (scheme_len > 0 && scheme_len < sizeof(ip_url)) {
            memcpy(ip_url, req->url, scheme_len);
            snprintf(ip_url + scheme_len, sizeof(ip_url) - scheme_len,
                     "%s%s", ip, uri);
            connect_url = ip_url;
        }
    }

    struct mg_connection *c = mg_http_connect(&mgr, connect_url, http_ev, &ctx);
    if (!c) {
        mg_mgr_free(&mgr);
        return MIMI_ERR_IO;
    }

    uint64_t start = mg_millis();
    uint32_t timeout = req->timeout_ms ? req->timeout_ms : 30000;
    while (!ctx.done) {
        mg_mgr_poll(&mgr, 10);
        if ((mg_millis() - start) > timeout) {
            ctx.result = MIMI_ERR_TIMEOUT;
            break;
        }
    }

    mg_mgr_free(&mgr);
    if (!ctx.done && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
    return ctx.result;
}

void mimi_http_response_free(mimi_http_response_t *resp)
{
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
    resp->status = 0;
}

