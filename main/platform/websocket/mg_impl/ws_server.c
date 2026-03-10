#include "../websocket.h"
#include "../../log.h"
#include "../../runtime.h"
#include "../../event/event_bus.h"

#include "mongoose.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct mimi_ws_server {
    struct mg_connection *listener;
    char path[32];
};

static const char *TAG = "ws_srv";

static void ws_server_ev(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        struct mimi_ws_server *srv = (struct mimi_ws_server *)c->fn_data;
        if (!srv) return;

        struct mg_str root = mg_str(srv->path);
        if (mg_strcmp(hm->uri, root) == 0) {
            mg_ws_upgrade(c, hm, NULL);

            event_bus_t *bus = event_bus_get_global();
            if (bus) {
                (void)event_bus_post_recv(bus, EVENT_CONNECT, (uint64_t)c, CONN_WS_SERVER,
                                          NULL, 0, 0);
            }
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;

        io_buf_t *buf = io_buf_from_const(wm->data.buf, wm->data.len);
        if (!buf) return;

        event_bus_t *bus = event_bus_get_global();
        if (bus) {
            (void)event_bus_post_recv(bus, EVENT_RECV, (uint64_t)c, CONN_WS_SERVER,
                                      buf, 0, 0);
        }
        io_buf_unref(buf);
    } else if (ev == MG_EV_CLOSE) {
        if (c->is_websocket) {
            event_bus_t *bus = event_bus_get_global();
            if (bus) {
                (void)event_bus_post_recv(bus, EVENT_DISCONNECT, (uint64_t)c, CONN_WS_SERVER,
                                          NULL, 0, 0);
            }
        }
    }
}

mimi_err_t mimi_ws_server_start(const mimi_ws_server_config_t *cfg, mimi_ws_server_t **out_server)
{
    if (!cfg || !out_server) return MIMI_ERR_INVALID_ARG;
    *out_server = NULL;

    struct mg_mgr *mgr = (struct mg_mgr *)mimi_runtime_get_event_loop();
    if (!mgr) return MIMI_ERR_INVALID_STATE;

    struct mimi_ws_server *srv = (struct mimi_ws_server *)calloc(1, sizeof(*srv));
    if (!srv) return MIMI_ERR_NO_MEM;

    const char *path = (cfg->path && cfg->path[0]) ? cfg->path : "/";
    strncpy(srv->path, path, sizeof(srv->path) - 1);
    srv->path[sizeof(srv->path) - 1] = '\0';

    char url[64];
    int port = (cfg->port > 0) ? cfg->port : 18789;
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", port);

    srv->listener = mg_http_listen(mgr, url, ws_server_ev, srv);
    if (!srv->listener) {
        free(srv);
        MIMI_LOGE(TAG, "Failed to listen on %s", url);
        return MIMI_ERR_IO;
    }

    *out_server = srv;
    MIMI_LOGI(TAG, "WS server listening on %s%s", url, srv->path);
    return MIMI_OK;
}

void mimi_ws_server_stop(mimi_ws_server_t *server)
{
    if (!server) return;
    if (server->listener) {
        mg_close_conn(server->listener);
        server->listener = NULL;
    }
    free(server);
}

uint64_t mimi_ws_server_get_listener_id(mimi_ws_server_t *server)
{
    if (!server || !server->listener) return 0;
    return (uint64_t)(uintptr_t)server->listener;
}

