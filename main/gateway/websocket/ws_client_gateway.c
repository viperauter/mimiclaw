/**
 * WebSocket Client Gateway Implementation
 *
 * Provides WebSocket client functionality using mongoose
 */

#include "gateway/websocket/ws_client_gateway.h"
#include "platform/log.h"
#include "platform/runtime.h"
#include "platform/websocket/websocket.h"
#include "platform/os/os.h"
#include "platform/mimi_time.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "gw_ws_client";

/* Global WebSocket Client Gateway instance */
static ws_client_gateway_priv_t s_ws_client_priv = {0};
static gateway_t g_ws_client_gateway = {0};

/* WebSocket event handler */
static void ws_client_event_handler(mimi_websocket_t *ws, mimi_ws_event_t event, 
                                   const uint8_t *data, size_t data_len, 
                                   void *user_data)
{
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)user_data;
    
    switch (event) {
        case MIMI_WS_EVENT_CONNECTED:
        {
            MIMI_LOGI(TAG, "WebSocket client connected to %s", priv->url);
            priv->connected = true;
            priv->reconnecting = false;
            
            /* Send authentication if token provided */
            if (priv->api_token[0] != '\0') {
                char auth_msg[256];
                snprintf(auth_msg, sizeof(auth_msg), "{\"type\":\"auth\",\"token\":\"%s\"}", priv->api_token);
                mimi_ws_send(ws, (const uint8_t *)auth_msg, strlen(auth_msg));
            }
            
            /* Call connect callback if set */
            if (priv->gateway && priv->gateway->on_connect_cb) {
                priv->gateway->on_connect_cb(priv->gateway, "default", 
                                           priv->gateway->callback_user_data);
            }
            break;
        }
        
        case MIMI_WS_EVENT_MESSAGE:
        {
            MIMI_LOGD(TAG, "Received WebSocket message: %.*s", (int)data_len, (const char *)data);
            
            /* Call gateway message callback if set */
            if (priv->gateway && priv->gateway->on_message_cb) {
                priv->gateway->on_message_cb(priv->gateway, "default", 
                                           (const char *)data, 
                                           priv->gateway->callback_user_data);
            }
            break;
        }
        
        case MIMI_WS_EVENT_DISCONNECTED:
        {
            MIMI_LOGI(TAG, "WebSocket client disconnected");
            priv->connected = false;
            
            /* Call disconnect callback if set */
            if (priv->gateway && priv->gateway->on_disconnect_cb) {
                priv->gateway->on_disconnect_cb(priv->gateway, "default", 
                                           priv->gateway->callback_user_data);
            }
            
            /* Auto reconnect */
            if (!priv->reconnecting) {
                priv->reconnecting = true;
                MIMI_LOGI(TAG, "Attempting to reconnect in 5 seconds...");
                mimi_sleep_ms(5000);
                ws_client_gateway_connect(priv->gateway);
            }
            break;
        }
        
        case MIMI_WS_EVENT_ERROR:
        {
            MIMI_LOGE(TAG, "WebSocket client error: %.*s", (int)data_len, (const char *)data);
            break;
        }
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
    }
    priv->timeout_ms = (ws_cfg && ws_cfg->timeout_ms > 0) ? ws_cfg->timeout_ms : 30000;
    priv->ping_interval_ms = (ws_cfg && ws_cfg->ping_interval_ms > 0) ? ws_cfg->ping_interval_ms : 30000;
    
    priv->gateway = gw;
    priv->initialized = true;
    priv->connected = false;
    priv->reconnecting = false;
    
    MIMI_LOGI(TAG, "WebSocket Client Gateway initialized (url: %s)", priv->url);
    return MIMI_OK;
}

static mimi_err_t ws_client_gateway_start_impl(gateway_t *gw)
{
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
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
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
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    ws_client_gateway_stop_impl(gw);
    
    memset(priv, 0, sizeof(ws_client_gateway_priv_t));
    MIMI_LOGI(TAG, "WebSocket Client Gateway destroyed");
}

static mimi_err_t ws_client_gateway_send_impl(gateway_t *gw, const char *session_id,
                                            const char *content)
{
    (void)session_id;
    ws_client_gateway_priv_t *priv = (ws_client_gateway_priv_t *)gw->priv_data;
    
    if (!priv->connected || !priv->ws) {
        MIMI_LOGW(TAG, "WebSocket client not connected");
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!content) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Send WebSocket message */
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
    
    MIMI_LOGI(TAG, "WebSocket Client Gateway module initialized");
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
    
    /* Close existing connection if any */
    if (priv->ws) {
        mimi_ws_destroy(priv->ws);
        priv->ws = NULL;
    }
    
    /* Create WebSocket configuration */
    mimi_ws_config_t config = {
        .url = priv->url,
        .headers = NULL,
        .timeout_ms = priv->timeout_ms,
        .ping_interval_ms = priv->ping_interval_ms,
        .event_cb = ws_client_event_handler,
        .user_data = priv
    };
    
    /* Create WebSocket client */
    priv->ws = mimi_ws_create(&config);
    if (!priv->ws) {
        MIMI_LOGE(TAG, "Failed to create WebSocket client");
        return MIMI_ERR_IO;
    }
    
    /* Connect to WebSocket server */
    mimi_err_t err = mimi_ws_connect(priv->ws);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to connect to WebSocket server: %d", err);
        mimi_ws_destroy(priv->ws);
        priv->ws = NULL;
        return err;
    }
    
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
