/**
 * WebSocket Channel Implementation
 *
 * Full implementation of WebSocket Server as a Channel.
 * No backward compatibility - all logic is self-contained.
 */

#include "channels/websocket/ws_channel.h"
#include "channels/channel_manager.h"
#include "commands/command.h"
#include "config.h"
#include "bus/message_bus.h"
#include "log.h"
#include "runtime.h"

#include "mongoose.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "websocket";

/* WebSocket client node */
typedef struct ws_client {
    struct mg_connection *c;
    char chat_id[32];
    struct ws_client *next;
} ws_client_t;

/* WebSocket Channel private data */
typedef struct {
    bool initialized;
    bool started;
    
    ws_client_t *clients;
    int port;
    
    /* Callbacks */
    void (*on_message)(channel_t *, const char *, const char *, void *);
    void (*on_connect)(channel_t *, const char *, void *);
    void (*on_disconnect)(channel_t *, const char *, void *);
    void *callback_user_data;
} ws_channel_priv_t;

static ws_channel_priv_t s_priv = {0};

/* Forward declarations */
static void ws_set_on_message_impl(channel_t *ch,
                                    void (*cb)(channel_t *, const char *,
                                               const char *, void *),
                                    void *user_data);
static void ws_set_on_connect_impl(channel_t *ch,
                                    void (*cb)(channel_t *, const char *,
                                               void *),
                                    void *user_data);
static void ws_set_on_disconnect_impl(channel_t *ch,
                                       void (*cb)(channel_t *, const char *,
                                                  void *),
                                       void *user_data);
static bool ws_is_running_impl(channel_t *ch);

/**
 * Find client by chat_id
 */
static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (ws_client_t *p = s_priv.clients; p; p = p->next) {
        if (strcmp(p->chat_id, chat_id) == 0) return p;
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
    snprintf(cl->chat_id, sizeof(cl->chat_id), "ws_%p", (void *)c);
    cl->next = s_priv.clients;
    s_priv.clients = cl;
    
    MIMI_LOGI(TAG, "Client connected: %s", cl->chat_id);
    
    /* Call connect callback if set */
    if (s_priv.on_connect) {
        channel_t *ch = &g_websocket_channel;
        s_priv.on_connect(ch, cl->chat_id, s_priv.callback_user_data);
    }
    
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
            
            MIMI_LOGI(TAG, "Client disconnected: %s", tmp->chat_id);
            
            /* Call disconnect callback if set */
            if (s_priv.on_disconnect) {
                channel_t *ch = &g_websocket_channel;
                s_priv.on_disconnect(ch, tmp->chat_id, s_priv.callback_user_data);
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
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        /* Upgrade HTTP connection to WebSocket when path is "/" */
        struct mg_str root = mg_str("/");
        if (mg_strcmp(hm->uri, root) == 0) {
            mg_ws_upgrade(c, hm, NULL);
            add_client(c);
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;

        char *payload = (char *)malloc((size_t)wm->data.len + 1);
        if (!payload) return;
        memcpy(payload, wm->data.buf, (size_t)wm->data.len);
        payload[wm->data.len] = '\0';

        ws_client_t *client = NULL;
        for (ws_client_t *p = s_priv.clients; p; p = p->next) {
            if (p->c == c) {
                client = p;
                break;
            }
        }

        cJSON *root = cJSON_Parse(payload);
        free(payload);
        if (!root) {
            MIMI_LOGW(TAG, "Invalid JSON from WS client");
            return;
        }

        cJSON *type = cJSON_GetObjectItem(root, "type");
        cJSON *content = cJSON_GetObjectItem(root, "content");

        if (type && cJSON_IsString(type) && strcmp(type->valuestring, "message") == 0 &&
            content && cJSON_IsString(content)) {

            const char *chat_id = client ? client->chat_id : "ws_unknown";
            cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
            if (cid && cJSON_IsString(cid)) {
                chat_id = cid->valuestring;
                if (client) {
                    strncpy(client->chat_id, chat_id, sizeof(client->chat_id) - 1);
                }
            }

            MIMI_LOGI(TAG, "WS message from %s: %.40s...", chat_id, content->valuestring);

            /* Call message callback if set */
            if (s_priv.on_message) {
                channel_t *ch = &g_websocket_channel;
                s_priv.on_message(ch, chat_id, content->valuestring, s_priv.callback_user_data);
            }

            /* Also push to message bus for backward compatibility during transition */
            mimi_msg_t msg = {0};
            strncpy(msg.channel, MIMI_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
            msg.content = strdup(content->valuestring);
            if (msg.content) {
                if (message_bus_push_inbound(&msg) != MIMI_OK) {
                    free(msg.content);
                }
            }
        }

        cJSON_Delete(root);
    } else if (ev == MG_EV_CLOSE) {
        if (c->is_websocket) {
            remove_client(c);
        }
    }
}

/**
 * Initialize WebSocket Channel
 */
mimi_err_t ws_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "WebSocket Channel already initialized");
        return MIMI_OK;
    }

    /* Load port from config */
    const mimi_config_t *config = mimi_config_get();
    s_priv.port = (config->ws_port > 0) ? config->ws_port : 18789;

    /* Store channel reference */
    ch->priv_data = &s_priv;
    s_priv.initialized = true;

    MIMI_LOGI(TAG, "WebSocket Channel initialized (port %d)", s_priv.port);
    return MIMI_OK;
}

/**
 * Start WebSocket Channel
 */
mimi_err_t ws_channel_start_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        MIMI_LOGE(TAG, "WebSocket Channel not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        MIMI_LOGW(TAG, "WebSocket Channel already started");
        return MIMI_OK;
    }

    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!mgr) {
        MIMI_LOGE(TAG, "Event loop not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", s_priv.port);
    if (!mg_http_listen(mgr, url, ws_ev, NULL)) {
        MIMI_LOGE(TAG, "Failed to start WebSocket server on %s", url);
        return MIMI_ERR_IO;
    }

    s_priv.started = true;
    MIMI_LOGI(TAG, "WebSocket Channel started on %s", url);
    return MIMI_OK;
}

/**
 * Stop WebSocket Channel
 */
mimi_err_t ws_channel_stop_impl(channel_t *ch)
{
    (void)ch;

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

    s_priv.started = false;
    MIMI_LOGI(TAG, "WebSocket Channel stopped");
    return MIMI_OK;
}

/**
 * Destroy WebSocket Channel
 */
void ws_channel_destroy_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return;
    }

    /* Stop first */
    ws_channel_stop_impl(ch);

    /* Clean up state */
    memset(&s_priv, 0, sizeof(s_priv));

    MIMI_LOGI(TAG, "WebSocket Channel destroyed");
}

/**
 * Send message through WebSocket Channel
 */
mimi_err_t ws_channel_send_impl(channel_t *ch, const char *session_id,
                                 const char *content)
{
    (void)ch;

    if (!s_priv.initialized || !s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    ws_client_t *client = find_client_by_chat_id(session_id);
    if (!client || !client->c) {
        MIMI_LOGW(TAG, "No WS client with chat_id=%s", session_id);
        return MIMI_ERR_NOT_FOUND;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", content);
    cJSON_AddStringToObject(resp, "chat_id", session_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str) return MIMI_ERR_NO_MEM;

    mg_ws_send(client->c, json_str, strlen(json_str), WEBSOCKET_OP_TEXT);
    free(json_str);
    return MIMI_OK;
}

/**
 * Check if channel is running
 */
static bool ws_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.initialized && s_priv.started;
}

/**
 * Set message callback
 */
static void ws_set_on_message_impl(channel_t *ch,
                                    void (*cb)(channel_t *, const char *,
                                               const char *, void *),
                                    void *user_data)
{
    (void)ch;
    s_priv.on_message = cb;
    s_priv.callback_user_data = user_data;
}

/**
 * Set connect callback
 */
static void ws_set_on_connect_impl(channel_t *ch,
                                    void (*cb)(channel_t *, const char *,
                                               void *),
                                    void *user_data)
{
    (void)ch;
    s_priv.on_connect = cb;
    s_priv.callback_user_data = user_data;
}

/**
 * Set disconnect callback
 */
static void ws_set_on_disconnect_impl(channel_t *ch,
                                       void (*cb)(channel_t *, const char *,
                                                  void *),
                                       void *user_data)
{
    (void)ch;
    s_priv.on_disconnect = cb;
    s_priv.callback_user_data = user_data;
}

/**
 * Initialize WebSocket Channel module
 * Called before registering with Channel Manager
 */
mimi_err_t ws_channel_init(void)
{
    /* Initialize the global channel structure */
    strncpy(g_websocket_channel.name, "websocket", sizeof(g_websocket_channel.name) - 1);
    g_websocket_channel.name[sizeof(g_websocket_channel.name) - 1] = '\0';
    
    strncpy(g_websocket_channel.description, "WebSocket Server Channel", 
            sizeof(g_websocket_channel.description) - 1);
    g_websocket_channel.description[sizeof(g_websocket_channel.description) - 1] = '\0';
    
    g_websocket_channel.require_auth = false;
    g_websocket_channel.max_sessions = -1;
    g_websocket_channel.init = ws_channel_init_impl;
    g_websocket_channel.start = ws_channel_start_impl;
    g_websocket_channel.stop = ws_channel_stop_impl;
    g_websocket_channel.destroy = ws_channel_destroy_impl;
    g_websocket_channel.send = ws_channel_send_impl;
    g_websocket_channel.is_running = ws_is_running_impl;
    g_websocket_channel.set_on_message = ws_set_on_message_impl;
    g_websocket_channel.set_on_connect = ws_set_on_connect_impl;
    g_websocket_channel.set_on_disconnect = ws_set_on_disconnect_impl;
    g_websocket_channel.priv_data = NULL;
    g_websocket_channel.is_initialized = false;
    g_websocket_channel.is_started = false;

    return MIMI_OK;
}

/* Global WebSocket channel instance */
channel_t g_websocket_channel = {
    .name = "websocket",
    .description = "WebSocket Server Channel",
    .require_auth = false,
    .max_sessions = -1,
    .init = ws_channel_init_impl,
    .start = ws_channel_start_impl,
    .stop = ws_channel_stop_impl,
    .destroy = ws_channel_destroy_impl,
    .send = ws_channel_send_impl,
    .is_running = ws_is_running_impl,
    .set_on_message = ws_set_on_message_impl,
    .set_on_connect = ws_set_on_connect_impl,
    .set_on_disconnect = ws_set_on_disconnect_impl,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false,
};
