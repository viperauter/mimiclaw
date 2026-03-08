#include "../websocket.h"
#include "../../log.h"

#include "mongoose.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* WebSocket client context */
typedef struct {
    mimi_ws_config_t config;
    mimi_ws_event_cb event_cb;
    void *user_data;
    
    struct mg_mgr mgr;
    struct mg_connection *conn;
    bool connected;
    
    uint64_t last_ping_time;
    uint32_t ping_interval_ms;
} mimi_ws_ctx_t;

struct mimi_websocket {
    mimi_ws_ctx_t ctx;
};

/* WebSocket event handler */
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
                if (ctx->event_cb) {
                    ctx->event_cb((mimi_websocket_t *)ctx, MIMI_WS_EVENT_ERROR, 
                                (const uint8_t *)"Connection failed", 17, 
                                ctx->user_data);
                }
                ctx->connected = false;
                return;
            }
            
            MIMI_LOGI("ws_mg", "WebSocket connected");
            ctx->connected = true;
            ctx->last_ping_time = mg_millis();
            
            if (ctx->event_cb) {
                ctx->event_cb((mimi_websocket_t *)ctx, MIMI_WS_EVENT_CONNECTED, 
                            NULL, 0, ctx->user_data);
            }
            break;
        }
        
        case MG_EV_WS_MSG:
        {
            struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
            if (ctx->event_cb) {
                ctx->event_cb((mimi_websocket_t *)ctx, MIMI_WS_EVENT_MESSAGE, 
                            (const uint8_t *)wm->data.buf, wm->data.len, 
                            ctx->user_data);
            }
            break;
        }
        
        case MG_EV_ERROR:
        {
            const char *err = (const char *)ev_data;
            MIMI_LOGE("ws_mg", "WebSocket error: %s", err ? err : "(unknown)");
            if (ctx->event_cb) {
                ctx->event_cb((mimi_websocket_t *)ctx, MIMI_WS_EVENT_ERROR, 
                            (const uint8_t *)err, err ? strlen(err) : 0, 
                            ctx->user_data);
            }
            break;
        }
        
        case MG_EV_CLOSE:
        {
            MIMI_LOGI("ws_mg", "WebSocket disconnected");
            ctx->connected = false;
            if (ctx->event_cb) {
                ctx->event_cb((mimi_websocket_t *)ctx, MIMI_WS_EVENT_DISCONNECTED, 
                            NULL, 0, ctx->user_data);
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
    
    /* Initialize Mongoose manager */
    mg_mgr_init(&ctx->mgr);
    
    return ws;
}

void mimi_ws_destroy(mimi_websocket_t *ws)
{
    if (!ws) return;
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    
    /* Disconnect if connected */
    if (ctx->conn) {
        mg_close_conn(ctx->conn);
    }
    
    /* Free Mongoose manager */
    mg_mgr_free(&ctx->mgr);
    
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
    }
    
    /* Create WebSocket connection */
    ctx->conn = mg_ws_connect(&ctx->mgr, ctx->config.url, ws_event_handler, ctx, ctx->config.headers);
    if (!ctx->conn) {
        MIMI_LOGE("ws_mg", "Failed to create WebSocket connection to %s", ctx->config.url);
        return MIMI_ERR_IO;
    }
    
    /* Set connection timeout */
    if (ctx->config.timeout_ms > 0) {
        uint64_t start = mg_millis();
        while (!ctx->connected) {
            mg_mgr_poll(&ctx->mgr, 10);
            if ((mg_millis() - start) > ctx->config.timeout_ms) {
                MIMI_LOGE("ws_mg", "WebSocket connection timeout");
                mg_close_conn(ctx->conn);
                ctx->conn = NULL;
                return MIMI_ERR_TIMEOUT;
            }
        }
    }
    
    return MIMI_OK;
}

void mimi_ws_disconnect(mimi_websocket_t *ws)
{
    if (!ws) return;
    
    mimi_ws_ctx_t *ctx = &ws->ctx;
    
    if (ctx->conn) {
        mg_close_conn(ctx->conn);
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
    
    mg_ws_send(ctx->conn, (const char *)data, data_len, WEBSOCKET_OP_TEXT);
    return MIMI_OK;
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
            mg_ws_send(ctx->conn, NULL, 0, WEBSOCKET_OP_PING);
            ctx->last_ping_time = now;
        }
    }
    
    /* Poll for events */
    mg_mgr_poll(&ctx->mgr, timeout_ms);
}