/**
 * WebSocket Client Gateway Implementation
 *
 * Provides WebSocket client functionality using mongoose
 */

#include "gateway/websocket/ws_client_gateway.h"
#include "log.h"
#include "runtime.h"
#include "websocket/websocket.h"
#include "os/os.h"
#include "event/event_bus.h"
#include "event/event_dispatcher.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "gw_ws_client";

/* Global WebSocket Client Gateway instance */
static ws_client_gateway_priv_t s_ws_client_priv = {0};
static gateway_t g_ws_client_gateway = {0};

/* Find priv by conn_id (user_data in event_msg) */
static ws_client_gateway_priv_t *find_priv_by_user_data(uint64_t user_data)
{
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)user_data;
    if (priv == &s_ws_client_priv) {
        return priv;
    }
    return NULL;
}

/* Event handler - runs in worker thread */
static void ws_client_event_handler(event_dispatcher_t *disp, event_msg_t *msg, void *user_data)
{
    (void)disp;
    (void)user_data;
    
    ws_client_gateway_priv_t *priv = find_priv_by_user_data(msg->user_data);
    if (!priv) {
        if (msg->buf) {
            io_buf_unref(msg->buf);
        }
        return;
    }
    
    switch (msg->type) {
        case EVENT_CONNECT:
            MIMI_LOGI(TAG, "WebSocket client connected to %s", priv->url);
            priv->connected = true;
            priv->reconnecting = false;
            
            /* Send authentication if token provided */
            if (priv->api_token[0] != '\0') {
                char auth_msg[256];
                snprintf(auth_msg, sizeof(auth_msg), "{\"type\":\"auth\",\"token\":\"%s\"}", priv->api_token);
                mimi_ws_send(priv->ws, (const uint8_t *)auth_msg, strlen(auth_msg));
            }
            
            /* Call connect callback if set */
            if (priv->gateway && priv->gateway->on_connect_cb) {
                priv->gateway->on_connect_cb(priv->gateway, "default", 
                                           priv->gateway->callback_user_data);
            }
            break;
            
        case EVENT_RECV:
            if (msg->buf && priv->gateway && priv->gateway->on_message_cb) {
                MIMI_LOGI(TAG, "Received WebSocket message");
                priv->gateway->on_message_cb(priv->gateway, "default", 
                                           (const char *)msg->buf->base,
                                           msg->buf->len,
                                           priv->gateway->callback_user_data);
            }
            break;
            
        case EVENT_DISCONNECT:
            MIMI_LOGI(TAG, "WebSocket client disconnected");
            priv->connected = false;
            
            /* Call disconnect callback if set */
            if (priv->gateway && priv->gateway->on_disconnect_cb) {
                priv->gateway->on_disconnect_cb(priv->gateway, "default", 
                                           priv->gateway->callback_user_data);
            }
            break;
            
        case EVENT_ERROR:
            MIMI_LOGE(TAG, "WebSocket client error: %d", msg->error_code);
            if (msg->buf) {
                MIMI_LOGE(TAG, "Error details: %.*s", 
                         (int)msg->buf->len, (const char *)msg->buf->base);
            }
            break;
            
        default:
            break;
    }
    
    if (msg->buf) {
        io_buf_unref(msg->buf);
    }
}

/* WebSocket Client Gateway implementation functions */

static mimi_err_t ws_client_gateway_init_impl(gateway_t *gw, const gateway_config_t *cfg)
{
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    if (priv->initialized) {
        MIMI_LOGW(TAG, "WebSocket Client Gateway already initialized");
        return MIMI_OK;
    }
    
    /* Store configuration - cast to specific config type */
    const ws_client_gateway_config_t *ws_cfg = (const ws_client_gateway_config_t *)cfg;
    if (ws_cfg && ws_cfg->url) {
        strncpy(priv->url, ws_cfg->url, sizeof(priv->url) - 1);
    }
    if (ws_cfg && ws_cfg->api_token) {
        strncpy(priv->api_token, ws_cfg->api_token, sizeof(priv->api_token) - 1);
        /* Build Authorization header */
        snprintf(priv->auth_header, sizeof(priv->auth_header), 
                 "Authorization: Bearer %s", ws_cfg->api_token);
    }
    priv->timeout_ms = (ws_cfg && ws_cfg->timeout_ms > 0) ? ws_cfg->timeout_ms : 30000;
    priv->ping_interval_ms = (ws_cfg && ws_cfg->ping_interval_ms > 0) ? ws_cfg->ping_interval_ms : 30000;
    
    priv->gateway = gw;
    priv->initialized = true;
    priv->connected = false;
    priv->reconnecting = false;
    gw->is_initialized = true;
    
    MIMI_LOGI(TAG, "WebSocket Client Gateway initialized (url: %s)", priv->url);
    return MIMI_OK;
}

static mimi_err_t ws_client_gateway_start_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    if (!priv->initialized) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (priv->connected) {
        return MIMI_OK;
    }
    
    /* Connect to WebSocket server */
    mimi_err_t err = ws_client_gateway_connect(gw);
    if (err != MIMI_OK) {
        return err;
    }
    
    MIMI_LOGI(TAG, "WebSocket Client Gateway started");
    return MIMI_OK;
}

static mimi_err_t ws_client_gateway_stop_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return MIMI_OK;
    }
    
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    /* Set stopping flag to prevent reconnection */
    priv->stopping = true;
    
    if (!priv->connected) {
        return MIMI_OK;
    }
    
    /* Disconnect from server */
    ws_client_gateway_disconnect(gw);
    
    MIMI_LOGI(TAG, "WebSocket Client Gateway stopped");
    return MIMI_OK;
}

static void ws_client_gateway_destroy_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return;
    }
    
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    ws_client_gateway_stop_impl(gw);
    
    memset(priv, 0, sizeof(ws_client_gateway_priv_t));
    MIMI_LOGI(TAG, "WebSocket Client Gateway destroyed");
}

static mimi_err_t ws_client_gateway_send_impl(gateway_t *gw, const char *session_id,
                                            const char *content)
{
    (void)session_id;
    
    if (!gw || !gw->priv_data) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    if (!priv->connected || !priv->ws) {
        MIMI_LOGW(TAG, "WebSocket client not connected");
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!content) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Send WebSocket message - will use send queue internally */
    mimi_err_t err = mimi_ws_send(priv->ws, (const uint8_t *)content, strlen(content));
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to send WebSocket message: %d", err);
        return err;
    }
    
    return MIMI_OK;
}

/* WebSocket Client Gateway module initialization */

mimi_err_t ws_client_gateway_module_init(void)
{
    memset(&s_ws_client_priv, 0, sizeof(s_ws_client_priv));
    
    /* Initialize gateway structure */
    strncpy(g_ws_client_gateway.name, "ws_client", sizeof(g_ws_client_gateway.name) - 1);
    g_ws_client_gateway.type = GATEWAY_TYPE_WS_CLIENT;
    g_ws_client_gateway.init = ws_client_gateway_init_impl;
    g_ws_client_gateway.start = ws_client_gateway_start_impl;
    g_ws_client_gateway.stop = ws_client_gateway_stop_impl;
    g_ws_client_gateway.destroy = ws_client_gateway_destroy_impl;
    g_ws_client_gateway.send = ws_client_gateway_send_impl;
    g_ws_client_gateway.set_on_message = NULL;
    g_ws_client_gateway.set_on_connect = NULL;
    g_ws_client_gateway.set_on_disconnect = NULL;
    g_ws_client_gateway.priv_data = &s_ws_client_priv;
    g_ws_client_gateway.is_initialized = false;
    g_ws_client_gateway.is_started = false;
    
    /* Register event handler with dispatcher */
    event_dispatcher_t *disp = (event_dispatcher_t *)mimi_runtime_get_dispatcher();
    if (disp) {
        event_dispatcher_register_handler(disp, CONN_WS_CLIENT, ws_client_event_handler, NULL);
    }
    
    MIMI_LOGD(TAG, "WebSocket Client Gateway module initialized");
    return MIMI_OK;
}

gateway_t* ws_client_gateway_get_instance(void)
{
    return &g_ws_client_gateway;
}

mimi_err_t ws_client_gateway_configure(const char *url, const char *api_token,
                                     int timeout_ms, int ping_interval_ms)
{
    ws_client_gateway_config_t cfg = {
        .url = url,
        .api_token = api_token,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 30000,
        .ping_interval_ms = ping_interval_ms > 0 ? ping_interval_ms : 30000
    };
    
    return ws_client_gateway_init_impl(&g_ws_client_gateway, (const gateway_config_t *)&cfg);
}

mimi_err_t ws_client_gateway_send_raw(const uint8_t *data, size_t len)
{
    ws_client_gateway_priv_t *priv = &s_ws_client_priv;
    
    if (!priv->initialized || !priv->connected || !priv->ws) {
        return MIMI_ERR_INVALID_STATE;
    }
    if (!data || len == 0) {
        return MIMI_ERR_INVALID_ARG;
    }

    /* Feishu expects protobuf Frame bytes as WS binary frames. */
    event_bus_t *bus = event_bus_get_global();
    if (!bus) return MIMI_ERR_INVALID_STATE;

    io_buf_t *buf = io_buf_from_const(data, len);
    if (!buf) return MIMI_ERR_NO_MEM;

    uint64_t conn_id = mimi_ws_get_conn_id(priv->ws);
    int ret = event_bus_post_send(bus, conn_id, CONN_WS_CLIENT, buf, EVENT_FLAG_BINARY);
    io_buf_unref(buf);

    return (ret == 0) ? MIMI_OK : MIMI_ERR_IO;
}

static void ws_connect_on_loop(void *arg)
{
    (void)mimi_ws_connect((mimi_ws_client_t *)arg);
}

mimi_err_t ws_client_gateway_connect(gateway_t *gw)
{
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    if (!priv->initialized) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (priv->url[0] == '\0') {
        MIMI_LOGE(TAG, "WebSocket URL not configured");
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Close existing connection if any (async close; will self-free on close) */
    if (priv->ws) {
        mimi_ws_destroy(priv->ws);
        priv->ws = NULL;
        priv->connected = false;
    }
    
    /* Create WebSocket configuration */
    mimi_ws_config_t config = {
        .url = priv->url,
        .headers = priv->auth_header[0] ? priv->auth_header : NULL,
        .timeout_ms = priv->timeout_ms,
        .ping_interval_ms = priv->ping_interval_ms,
        .event_cb = NULL,  /* Not used anymore - events go through queue */
        .user_data = priv
    };
    
    /* Create WebSocket client */
    priv->ws = mimi_ws_create(&config);
    if (!priv->ws) {
        MIMI_LOGE(TAG, "Failed to create WebSocket client");
        return MIMI_ERR_IO;
    }
    
    /* Set connection type for event messages */
    mimi_ws_set_conn_type(priv->ws, CONN_WS_CLIENT);
    
    /* Connect to WebSocket server MUST run in runtime/event-loop thread (mongoose is not thread-safe). */
    event_bus_t *bus = event_bus_get_global();
    if (!bus) {
        mimi_ws_destroy(priv->ws);
        priv->ws = NULL;
        return MIMI_ERR_INVALID_STATE;
    }

    mimi_ws_client_t *ws = priv->ws;
    (void)event_bus_post_call(bus, ws_connect_on_loop, ws);
    MIMI_LOGI(TAG, "Connecting to WebSocket server: %s", priv->url);
    return MIMI_OK;
}

mimi_err_t ws_client_gateway_disconnect(gateway_t *gw)
{
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    if (!priv->ws) {
        return MIMI_OK;
    }
    
    /* Close WebSocket connection */
    mimi_ws_destroy(priv->ws);
    priv->ws = NULL;
    priv->connected = false;
    
    MIMI_LOGI(TAG, "Disconnected from WebSocket server");
    return MIMI_OK;
}

mimi_err_t ws_client_gateway_send(gateway_t *gw, const char *session_id,
                                  const char *content)
{
    return ws_client_gateway_send_impl(gw, session_id, content);
}

bool ws_client_gateway_is_connected(gateway_t *gw)
{
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    return priv && priv->connected;
}

void ws_client_gateway_set_channel_data(gateway_t *gw, void *data)
{
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    if (priv) {
        priv->channel_data = data;
    }
}

void* ws_client_gateway_get_channel_data(gateway_t *gw)
{
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    return priv ? priv->channel_data : NULL;
}
