/**
 * WebSocket Gateway Implementation
 *
 * Provides WebSocket server transport using mongoose
 * Adapted from channels/websocket/ws_channel.c
 */

#include "gateway/websocket/ws_gateway.h"
#include "router/router.h"
#include "platform/log.h"
#include "platform/runtime.h"

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

            /* Call gateway disconnect callback */
            gateway_t *gw = &g_ws_gateway;
            if (gw->on_disconnect_cb) {
                gw->on_disconnect_cb(gw, tmp->session_id, gw->callback_user_data);
            }

            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

/**
 * WebSocket event handler
 */
static void ws_ev(struct mg_connection *c, int ev, void *ev_data)
{
    gateway_t *gw = &g_ws_gateway;

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        struct mg_str root = mg_str(s_priv.path);
        if (mg_strcmp(hm->uri, root) == 0) {
            mg_ws_upgrade(c, hm, NULL);
            ws_client_t *cl = add_client(c);

            /* Call connect callback */
            if (cl && gw->on_connect_cb) {
                gw->on_connect_cb(gw, cl->session_id, gw->callback_user_data);
            }
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;

        char *payload = (char *)malloc((size_t)wm->data.len + 1);
        if (!payload) return;
        memcpy(payload, wm->data.buf, (size_t)wm->data.len);
        payload[wm->data.len] = '\0';

        ws_client_t *client = find_client_by_conn(c);
        if (!client) {
            free(payload);
            return;
        }

        cJSON *root = cJSON_Parse(payload);
        free(payload);
        if (!root) {
            MIMI_LOGW(TAG, "Invalid JSON from WS client %s", client->session_id);
            return;
        }

        cJSON *type = cJSON_GetObjectItem(root, "type");
        cJSON *content = cJSON_GetObjectItem(root, "content");

        if (type && cJSON_IsString(type) &&
            content && cJSON_IsString(content)) {

            /* Update session_id if provided */
            cJSON *sid = cJSON_GetObjectItem(root, "session_id");
            if (sid && cJSON_IsString(sid)) {
                strncpy(client->session_id, sid->valuestring,
                        sizeof(client->session_id) - 1);
            }

            MIMI_LOGI(TAG, "WS message from %s: %.40s...",
                      client->session_id, content->valuestring);

            /* Call message callback */
            if (gw->on_message_cb) {
                gw->on_message_cb(gw, client->session_id,
                                  content->valuestring,
                                  gw->callback_user_data);
            }
        }

        cJSON_Delete(root);
    } else if (ev == MG_EV_CLOSE) {
        if (c->is_websocket) {
            remove_client(c);
        }
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

    mg_ws_send(client->c, json_str, strlen(json_str), WEBSOCKET_OP_TEXT);
    free(json_str);
    return MIMI_OK;
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
