/**
 * WebSocket Gateway Implementation
 *
 * Provides WebSocket server transport using mongoose
 * Adapted from channels/websocket/ws_channel.c
 */

#include "gateway/websocket/ws_gateway.h"
#include "router/router.h"
#include "log.h"
#include "runtime.h"
#include "event/event_bus.h"
#include "event/event_dispatcher.h"

#include "mongoose.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "gw_ws";

/* WebSocket client node */
typedef struct ws_client {
    struct mg_connection *c;
    char session_id[32];
    struct ws_client *next;
} ws_client_t;

/* WebSocket Gateway private data */
typedef struct {
    bool initialized;
    bool started;
    int port;
    char path[32];
    ws_client_t *clients;
    struct mg_connection *listener;
} ws_gateway_priv_t;

static ws_gateway_priv_t s_priv = {
    .port = 18789,
    .path = "/"
};

/* Forward declarations */
static void ws_ev(struct mg_connection *c, int ev, void *ev_data);

/**
 * Find client by session_id
 */
static ws_client_t *find_client_by_session_id(const char *session_id)
{
    for (ws_client_t *p = s_priv.clients; p; p = p->next) {
        if (strcmp(p->session_id, session_id) == 0) return p;
    }
    return NULL;
}

/**
 * Find client by connection
 */
static ws_client_t *find_client_by_conn(struct mg_connection *c)
{
    for (ws_client_t *p = s_priv.clients; p; p = p->next) {
        if (p->c == c) return p;
    }
    return NULL;
}

/**
 * Add new client
 */
static ws_client_t *add_client(struct mg_connection *c)
{
    ws_client_t *cl = (ws_client_t *)calloc(1, sizeof(ws_client_t));
    if (!cl) return NULL;
    cl->c = c;
    snprintf(cl->session_id, sizeof(cl->session_id), "ws_%p", (void *)c);
    cl->next = s_priv.clients;
    s_priv.clients = cl;

    MIMI_LOGI(TAG, "Client connected: %s", cl->session_id);
    return cl;
}

/**
 * Remove client
 */
static void remove_client(struct mg_connection *c)
{
    ws_client_t **pp = &s_priv.clients;
    while (*pp) {
        if ((*pp)->c == c) {
            ws_client_t *tmp = *pp;
            *pp = (*pp)->next;

            MIMI_LOGI(TAG, "Client disconnected: %s", tmp->session_id);

            /* Post disconnect event to recv queue */
            event_bus_t *bus = event_bus_get_global();
            if (bus) {
                event_bus_post_recv(bus, EVENT_DISCONNECT, (uint64_t)c, CONN_WS_SERVER, NULL, (uint64_t)tmp->session_id, 0);
            }

            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

/**
 * WebSocket event handler - runs in event loop thread
 */
static void ws_ev(struct mg_connection *c, int ev, void *ev_data)
{
    (void)c;

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        struct mg_str root = mg_str(s_priv.path);
        if (mg_strcmp(hm->uri, root) == 0) {
            mg_ws_upgrade(c, hm, NULL);
            ws_client_t *cl = add_client(c);

            /* Post connect event to recv queue */
            if (cl) {
                event_bus_t *bus = event_bus_get_global();
                if (bus) {
                    event_bus_post_recv(bus, EVENT_CONNECT, (uint64_t)c, CONN_WS_SERVER, NULL, (uint64_t)cl->session_id, 0);
                }
            }
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;

        ws_client_t *client = find_client_by_conn(c);
        if (!client) {
            return;
        }

        /* Create io_buf from message data */
        io_buf_t *buf = io_buf_from_const(wm->data.buf, wm->data.len);
        if (!buf) {
            return;
        }

        /* Post recv event to queue - NON-BLOCKING */
        event_bus_t *bus = event_bus_get_global();
        if (bus) {
            event_bus_post_recv(bus, EVENT_RECV, (uint64_t)c, CONN_WS_SERVER, buf, (uint64_t)client->session_id, 0);
        }

        /* Unref our reference - queue holds one */
        io_buf_unref(buf);
    } else if (ev == MG_EV_CLOSE) {
        if (c->is_websocket) {
            remove_client(c);
        }
    }
}

/* Event handler - runs in worker thread */
static void ws_server_event_handler(event_dispatcher_t *disp, event_msg_t *msg, void *user_data)
{
    (void)disp;
    (void)user_data;
    gateway_t *gw = &g_ws_gateway;
    
    char *session_id = (char *)msg->user_data;
    
    switch (msg->type) {
        case EVENT_CONNECT:
            if (session_id && gw->on_connect_cb) {
                gw->on_connect_cb(gw, session_id, gw->callback_user_data);
            }
            break;
            
        case EVENT_RECV: {
            if (!msg->buf || !session_id) {
                break;
            }
            
            char *payload = (char *)malloc(msg->buf->len + 1);
            if (!payload) {
                break;
            }
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

                MIMI_LOGI(TAG, "WS message from %s: %.40s...",
                          session_id, content->valuestring);

                /* Call message callback */
                if (gw->on_message_cb) {
                    gw->on_message_cb(gw, session_id,
                                      content->valuestring,
                                      gw->callback_user_data);
                }
            }

            cJSON_Delete(root);
            break;
        }
            
        case EVENT_DISCONNECT:
            if (session_id && gw->on_disconnect_cb) {
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
    MIMI_LOGI(TAG, "WebSocket Gateway initialized (port %d)", s_priv.port);
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

    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!mgr) {
        MIMI_LOGE(TAG, "Event loop not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", s_priv.port);
    s_priv.listener = mg_http_listen(mgr, url, ws_ev, NULL);
    if (!s_priv.listener) {
        MIMI_LOGE(TAG, "Failed to start WebSocket server on %s", url);
        return MIMI_ERR_IO;
    }

    s_priv.started = true;
    MIMI_LOGI(TAG, "WebSocket Gateway started on %s", url);
    return MIMI_OK;
}

static mimi_err_t ws_gateway_stop_impl(gateway_t *gw)
{
    (void)gw;

    if (!s_priv.initialized) {
        return MIMI_OK;
    }

    /* Clean up client list */
    ws_client_t *p = s_priv.clients;
    while (p) {
        ws_client_t *next = p->next;
        free(p);
        p = next;
    }
    s_priv.clients = NULL;

    s_priv.listener = NULL;
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

    ws_client_t *client = find_client_by_session_id(session_id);
    if (!client || !client->c) {
        MIMI_LOGW(TAG, "No WS client with session_id=%s", session_id);
        return MIMI_ERR_NOT_FOUND;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", content);
    cJSON_AddStringToObject(resp, "session_id", session_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str) return MIMI_ERR_NO_MEM;

    /* Create io_buf and post to send queue */
    io_buf_t *buf = io_buf_from_const(json_str, strlen(json_str));
    free(json_str);
    
    if (!buf) {
        return MIMI_ERR_NO_MEM;
    }

    /* Post to send queue - will be processed in event loop */
    event_bus_t *bus = event_bus_get_global();
    int ret = -1;
    if (bus) {
        ret = event_bus_post_send(bus, (uint64_t)client->c, CONN_WS_SERVER, buf, 0);
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
