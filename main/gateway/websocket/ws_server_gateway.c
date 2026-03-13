/**
 * WebSocket Server Gateway Implementation
 *
 * Provides WebSocket server transport via platform websocket server.
 */

#include "gateway/websocket/ws_server_gateway.h"
#include "log.h"
#include "runtime.h"
#include "event/event_bus.h"
#include "event/event_dispatcher.h"
#include "platform/websocket/websocket.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"

static const char *TAG = "gw_ws";

/* WebSocket Gateway private data */
typedef struct {
    bool initialized;
    bool started;
    int port;
    char path[32];
    mimi_ws_server_t *server;
} ws_gateway_priv_t;

static ws_gateway_priv_t s_priv = {
    .port = 18789,
    .path = "/"
};

static void ws_make_session_id(uint64_t conn_id, char *out, size_t out_len)
{
    snprintf(out, out_len, "ws_%p", (void *)(uintptr_t)conn_id);
}

static uint64_t ws_parse_session_id(const char *session_id)
{
    if (!session_id) return 0;
    void *p = NULL;
    /* Accept "ws_%p" */
    if (sscanf(session_id, "ws_%p", &p) == 1) {
        return (uint64_t)(uintptr_t)p;
    }
    return 0;
}

/* Event handler - runs in worker thread */
static void ws_server_event_handler(event_dispatcher_t *disp, event_msg_t *msg, void *user_data)
{
    (void)disp;
    (void)user_data;
    gateway_t *gw = &g_ws_gateway;
    char session_id[32];
    ws_make_session_id(msg->conn_id, session_id, sizeof(session_id));
    
    switch (msg->type) {
        case EVENT_CONNECT:
            if (gw->on_connect_cb) {
                gw->on_connect_cb(gw, session_id, gw->callback_user_data);
            }
            break;
            
        case EVENT_RECV: {
            if (!msg->buf) {
                break;
            }
            char *payload = (char *)malloc(msg->buf->len + 1);
            if (!payload) break;
            memcpy(payload, msg->buf->base, msg->buf->len);
            payload[msg->buf->len] = '\0';

            cJSON *root = cJSON_Parse(payload);
            free(payload);
            if (!root) {
                MIMI_LOGW(TAG, "Invalid JSON from WS client %s", session_id);
                break;
            }

            cJSON *type = cJSON_GetObjectItem(root, "type");
            cJSON *content = cJSON_GetObjectItem(root, "content");

            if (type && cJSON_IsString(type) &&
                content && cJSON_IsString(content)) {
                if (gw->on_message_cb) {
                    const char *msg_str = content->valuestring;
                    size_t msg_len = msg_str ? strlen(msg_str) : 0;
                    gw->on_message_cb(gw, session_id,
                                      msg_str,
                                      msg_len,
                                      gw->callback_user_data);
                }
            }

            cJSON_Delete(root);
            break;
        }
            
        case EVENT_DISCONNECT:
            if (gw->on_disconnect_cb) {
                gw->on_disconnect_cb(gw, session_id, gw->callback_user_data);
            }
            break;
            
        default:
            break;
    }
    
    if (msg->buf) {
        io_buf_unref(msg->buf);
    }
}

/* Gateway implementation functions */

static mimi_err_t ws_gateway_init_impl(gateway_t *gw, const gateway_config_t *cfg)
{
    (void)gw;
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "WebSocket Gateway already initialized");
        return MIMI_OK;
    }

    s_priv.initialized = true;
    MIMI_LOGD(TAG, "WebSocket Gateway initialized (port %d)", s_priv.port);
    return MIMI_OK;
}

static mimi_err_t ws_gateway_start_impl(gateway_t *gw)
{
    (void)gw;

    if (!s_priv.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        return MIMI_OK;
    }

    mimi_ws_server_config_t cfg = {
        .port = s_priv.port,
        .path = s_priv.path
    };
    mimi_err_t err = mimi_ws_server_start(&cfg, &s_priv.server);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start WS server: %d", err);
        return err;
    }

    s_priv.started = true;
    MIMI_LOGI(TAG, "WebSocket Gateway started (port=%d path=%s)", s_priv.port, s_priv.path);
    return MIMI_OK;
}

static mimi_err_t ws_gateway_stop_impl(gateway_t *gw)
{
    (void)gw;

    if (!s_priv.initialized) {
        return MIMI_OK;
    }

    if (s_priv.server) {
        mimi_ws_server_stop(s_priv.server);
        s_priv.server = NULL;
    }
    s_priv.started = false;
    MIMI_LOGI(TAG, "WebSocket Gateway stopped");
    return MIMI_OK;
}

static void ws_gateway_destroy_impl(gateway_t *gw)
{
    (void)gw;
    ws_gateway_stop_impl(gw);
    memset(&s_priv, 0, sizeof(s_priv));
    s_priv.port = 18789;
    strncpy(s_priv.path, "/", sizeof(s_priv.path) - 1);
    MIMI_LOGI(TAG, "WebSocket Gateway destroyed");
}

static mimi_err_t ws_gateway_send_impl(gateway_t *gw, const char *session_id,
                                        const char *content)
{
    (void)gw;

    if (!s_priv.initialized || !s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    uint64_t conn_id = ws_parse_session_id(session_id);
    if (!conn_id) {
        MIMI_LOGW(TAG, "Invalid session_id=%s", session_id);
        return MIMI_ERR_INVALID_ARG;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", content);
    cJSON_AddStringToObject(resp, "session_id", session_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str) return MIMI_ERR_NO_MEM;

    io_buf_t *buf = io_buf_from_const(json_str, strlen(json_str));
    free(json_str);
    if (!buf) return MIMI_ERR_NO_MEM;

    event_bus_t *bus = event_bus_get_global();
    int ret = -1;
    if (bus) {
        ret = event_bus_post_send(bus, conn_id, CONN_WS_SERVER, buf, 0);
    }
    io_buf_unref(buf);

    return (ret == 0) ? MIMI_OK : MIMI_ERR_IO;
}

/* Global WebSocket Gateway instance */
gateway_t g_ws_gateway = {
    .name = "websocket",
    .type = GATEWAY_TYPE_WS_SERVER,
    .init = ws_gateway_init_impl,
    .start = ws_gateway_start_impl,
    .stop = ws_gateway_stop_impl,
    .destroy = ws_gateway_destroy_impl,
    .send = ws_gateway_send_impl,
    .set_on_message = NULL,
    .set_on_connect = NULL,
    .set_on_disconnect = NULL,
    .is_initialized = false,
    .is_started = false,
    .priv_data = NULL,
    .on_message_cb = NULL,
    .on_connect_cb = NULL,
    .on_disconnect_cb = NULL,
    .callback_user_data = NULL
};

mimi_err_t ws_gateway_module_init(void)
{
    /* Register event handler with dispatcher */
    event_dispatcher_t *disp = (event_dispatcher_t *)mimi_runtime_get_dispatcher();
    if (disp) {
        event_dispatcher_register_handler(disp, CONN_WS_SERVER, ws_server_event_handler, NULL);
    }
    
    return MIMI_OK;
}

gateway_t* ws_gateway_get(void)
{
    return &g_ws_gateway;
}

mimi_err_t ws_gateway_configure(int port, const char *path)
{
    if (s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }

    s_priv.port = (port > 0) ? port : 18789;
    if (path) {
        strncpy(s_priv.path, path, sizeof(s_priv.path) - 1);
        s_priv.path[sizeof(s_priv.path) - 1] = '\0';
    }

    return MIMI_OK;
}

