#include "../websocket.h"
#include "../../log.h"
#include "../../runtime.h"
#include "../../mimi_time.h"
#include "../../event/event_bus.h"
#include "../../event/event_dispatcher.h"
#include "mongoose.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* WebSocket client context */
typedef struct {
    mimi_ws_config_t config;
    mimi_ws_event_cb event_cb;
    void *user_data;
    
    struct mg_connection *conn;
    bool connected;
    
    uint64_t last_ping_time;
    uint32_t ping_interval_ms;
    
    /* Connection type for event messages */
    conn_type_t conn_type;
} mimi_ws_ctx_t;

struct mimi_websocket {
    mimi_ws_ctx_t ctx;
};

/* WebSocket event handler - runs in event loop thread */
static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data)
{
    mimi_ws_ctx_t *ctx = (mimi_ws_ctx_t *)c->fn_data;
    if (!ctx) return;
    
    switch (ev) {
        case MG_EV_CONNECT:
        {
            int status = 0;
            if (ev_data != NULL) status = *(int *)ev_data;
            if (status != 0) {
                MIMI_LOGE("ws_mg", "WebSocket connect failed (status=%d)", status);
                event_bus_t *bus = event_bus_get_global();
                if (bus) {
                    event_bus_post_error(bus, (uint64_t)c, ctx->conn_type, status, (uint64_t)ctx->user_data, 0);
                }
                ctx->connected = false;
                return;
            }
            
            MIMI_LOGI("ws_mg", "WebSocket connected");
            ctx->connected = true;
            
            /* Post connect event to worker thread */
            event_bus_t *bus = event_bus_get_global();
            if (bus) {
                event_bus_post_recv(bus, EVENT_CONNECT, (uint64_t)c, ctx->conn_type, NULL, (uint64_t)ctx->user_data, 0);
            }
            break;
        }
        
        case MG_EV_WS_MSG:
        {
            struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
            
            /* Create io_buf from message data */
            io_buf_t *buf = io_buf_from_const(wm->data.buf, wm->data.len);
            if (!buf) {
                MIMI_LOGW("ws_mg", "Failed to create io_buf for WS message");
                return;
            }
            
            /* Post recv event to queue - NON-BLOCKING */
            event_bus_t *bus = event_bus_get_global();
            if (bus) {
                event_bus_post_recv(bus, EVENT_RECV, (uint64_t)c, ctx->conn_type, buf, (uint64_t)ctx->user_data, 0);
            }
            
            /* Unref our reference - queue holds one */
            io_buf_unref(buf);
            break;
        }
        
        case MG_EV_ERROR:
        {
            const char *err = (const char *)ev_data;
            MIMI_LOGE("ws_mg", "WebSocket error: %s", err ? err : "(unknown)");
            
            /* Post error event */
            io_buf_t *buf = io_buf_from_const(err, err ? strlen(err) : 0);
            event_bus_t *bus = event_bus_get_global();
            if (bus) {
                event_bus_post_recv(bus, EVENT_ERROR, (uint64_t)c, ctx->conn_type, buf, (uint64_t)ctx->user_data, 0);
            }
            if (buf) io_buf_unref(buf);
            break;
        }
        
        case MG_EV_CLOSE:
        {
            MIMI_LOGI("ws_mg", "WebSocket disconnected");
            ctx->connected = false;
            
            /* Post disconnect event */
            event_bus_t *bus = event_bus_get_global();
            if (bus) {
                event_bus_post_recv(bus, EVENT_DISCONNECT, (uint64_t)c, ctx->conn_type, NULL, (uint64_t)ctx->user_data, 0);
            }
            break;
        }
    }
}

mimi_websocket_t *mimi_ws_create(const mimi_ws_config_t *config)
{
    if (!config || !config->url) {
        return NULL;
    }
    
    mimi_websocket_t *ws = (mimi_websocket_t *)malloc(sizeof(mimi_websocket_t));
    if (!ws) {
        return NULL;
    }
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    memset(ctx, 0, sizeof(mimi_ws_ctx_t));
    
    /* Copy configuration */
    ctx->config = *config;
    ctx->event_cb = config->event_cb;
    ctx->user_data = config->user_data;
    
    ctx->ping_interval_ms = config->ping_interval_ms > 0 ? 
                           config->ping_interval_ms : 30000;
    
    /* Default connection type */
    ctx->conn_type = CONN_WS_CLIENT;
    
    return ws;
}

void mimi_ws_destroy(mimi_websocket_t *ws)
{
    if (!ws) return;
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    
    /* Disconnect if connected */
    if (ctx->conn) {
        mg_close_conn(ctx->conn);
        ctx->conn = NULL;
    }
    
    free(ws);
}

mimi_err_t mimi_ws_connect(mimi_websocket_t *ws)
{
    if (!ws) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    
    /* Close existing connection if any */
    if (ctx->conn) {
        mg_close_conn(ctx->conn);
        ctx->conn = NULL;
    }
    
    /* Create WebSocket connection */
    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!mgr) {
        MIMI_LOGE("ws_mg", "Failed to get runtime event loop");
        return MIMI_ERR_INVALID_STATE;
    }

    ctx->conn = mg_ws_connect(mgr, ctx->config.url, ws_event_handler, ctx, ctx->config.headers);
    if (!ctx->conn) {
        MIMI_LOGE("ws_mg", "Failed to create WebSocket connection to %s", ctx->config.url);
        return MIMI_ERR_IO;
    }

    /* If using TLS (wss://), configure SNI based on original hostname.
     * This mirrors the HTTP client TLS setup and is required to avoid
     * "plain HTTP request was sent to HTTPS port" errors. */
    if (mg_url_is_ssl(ctx->config.url)) {
        struct mg_str host = mg_url_host(ctx->config.url);
        char host_name[128] = {0};
        if (host.len > 0 && host.len < (int)sizeof(host_name)) {
            memcpy(host_name, host.buf, host.len);
            host_name[host.len] = '\0';

            struct mg_tls_opts opts;
            memset(&opts, 0, sizeof(opts));
            opts.name = mg_str(host_name);   /* SNI / hostname verification */
            opts.skip_verification = 1;      /* dev env: skip CA verification */
            mg_tls_init(ctx->conn, &opts);

            MIMI_LOGI("ws_mg", "Initialized TLS for WS with host=%s", host_name);
        }
    }

    return MIMI_OK;
}

void mimi_ws_disconnect(mimi_websocket_t *ws)
{
    if (!ws) return;
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    
    if (ctx->conn) {
        /* Post close request to send queue */
        event_bus_t *bus = event_bus_get_global();
        if (bus) {
            event_bus_post_close(bus, (uint64_t)ctx->conn, ctx->conn_type, 0);
        }
        ctx->conn = NULL;
        ctx->connected = false;
    }
}

mimi_err_t mimi_ws_send(mimi_websocket_t *ws, const uint8_t *data, size_t data_len)
{
    if (!ws || !data || data_len == 0) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    
    if (!ctx->conn || !ctx->connected) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    /* Create io_buf from data */
    io_buf_t *buf = io_buf_from_const(data, data_len);
    if (!buf) {
        return MIMI_ERR_NO_MEM;
    }
    
    /* Post send request to send queue - will be processed in event loop */
    event_bus_t *bus = event_bus_get_global();
    int ret = -1;
    if (bus) {
        ret = event_bus_post_send(bus, (uint64_t)ctx->conn, ctx->conn_type, buf, 0);
    }
    
    /* Unref our reference - queue holds one */
    io_buf_unref(buf);
    
    return (ret == 0) ? MIMI_OK : MIMI_ERR_IO;
}

bool mimi_ws_is_connected(mimi_websocket_t *ws)
{
    if (!ws) return false;
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    return ctx->connected;
}

void mimi_ws_poll(mimi_websocket_t *ws, uint32_t timeout_ms)
{
    if (!ws) return;
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    
    /* Send ping if needed */
    if (ctx->connected && ctx->ping_interval_ms > 0) {
        uint64_t now = mg_millis();
        if (now - ctx->last_ping_time > ctx->ping_interval_ms) {
            /* Post ping as send request */
            event_bus_t *bus = event_bus_get_global();
            if (bus) {
                event_bus_post_send(bus, (uint64_t)ctx->conn, ctx->conn_type, NULL, 0);
            }
            ctx->last_ping_time = now;
        }
    }
    
    /* Event loop is handled by runtime, no need to poll here */
    /* Just sleep to avoid busy loop if called directly */
    if (timeout_ms > 0) {
        mimi_sleep_ms(timeout_ms);
    }
}

void mimi_ws_set_conn_type(mimi_websocket_t *ws, conn_type_t conn_type)
{
    if (ws) {
        ws->ctx.conn_type = conn_type;
    }
}

conn_type_t mimi_ws_get_conn_type(mimi_websocket_t *ws)
{
    return ws ? ws->ctx.conn_type : CONN_UNKNOWN;
}

uint64_t mimi_ws_get_conn_id(mimi_websocket_t *ws)
{
    return ws ? (uint64_t)ws->ctx.conn : 0;
}
