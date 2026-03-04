#include "gateway/ws_server.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "platform/log.h"

#include "mongoose.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws_posix";

typedef struct ws_client {
    struct mg_connection *c;
    char chat_id[32];
    struct ws_client *next;
} ws_client_t;

static struct mg_mgr *s_mgr = NULL;
static ws_client_t *s_clients = NULL;

void ws_server_set_mgr(struct mg_mgr *mgr)
{
    s_mgr = mgr;
}

static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (ws_client_t *p = s_clients; p; p = p->next) {
        if (strcmp(p->chat_id, chat_id) == 0) return p;
    }
    return NULL;
}

static ws_client_t *add_client(struct mg_connection *c)
{
    ws_client_t *cl = (ws_client_t *)calloc(1, sizeof(ws_client_t));
    if (!cl) return NULL;
    cl->c = c;
    snprintf(cl->chat_id, sizeof(cl->chat_id), "ws_%p", (void *)c);
    cl->next = s_clients;
    s_clients = cl;
    MIMI_LOGI(TAG, "Client connected: %s", cl->chat_id);
    return cl;
}

static void remove_client(struct mg_connection *c)
{
    ws_client_t **pp = &s_clients;
    while (*pp) {
        if ((*pp)->c == c) {
            ws_client_t *tmp = *pp;
            *pp = (*pp)->next;
            MIMI_LOGI(TAG, "Client disconnected: %s", tmp->chat_id);
            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

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
        for (ws_client_t *p = s_clients; p; p = p->next) {
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

mimi_err_t ws_server_start(void)
{
    if (!s_mgr) {
        MIMI_LOGE(TAG, "ws_server_start: mg_mgr not set");
        return MIMI_ERR_INVALID_STATE;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", MIMI_WS_PORT);
    if (!mg_http_listen(s_mgr, url, ws_ev, NULL)) {
        MIMI_LOGE(TAG, "Failed to start WebSocket server on %s", url);
        return MIMI_ERR_IO;
    }

    MIMI_LOGI(TAG, "WebSocket server started on %s", url);
    return MIMI_OK;
}

mimi_err_t ws_server_send(const char *chat_id, const char *text)
{
    if (!s_mgr) return MIMI_ERR_INVALID_STATE;
    if (!chat_id || !text) return MIMI_ERR_INVALID_ARG;

    ws_client_t *client = find_client_by_chat_id(chat_id);
    if (!client || !client->c) {
        MIMI_LOGW(TAG, "No WS client with chat_id=%s", chat_id);
        return MIMI_ERR_NOT_FOUND;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str) return MIMI_ERR_NO_MEM;

    mg_ws_send(client->c, json_str, strlen(json_str), WEBSOCKET_OP_TEXT);
    free(json_str);
    return MIMI_OK;
}

mimi_err_t ws_server_stop(void)
{
    /* Connections will be closed when mg_mgr is freed in main_posix. */
    ws_client_t *p = s_clients;
    while (p) {
        ws_client_t *next = p->next;
        free(p);
        p = next;
    }
    s_clients = NULL;
    return MIMI_OK;
}

