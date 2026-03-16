/**
 * WebSocket Server Channel Implementation
 *
 * Uses the WebSocket Server Gateway for transport.
 * Routes incoming WebSocket messages through the Input Processor.
 */

#include "channels/ws_service/ws_service_channel.h"
#include "channels/channel_manager.h"
#include "gateway/gateway_manager.h"
#include "gateway/websocket/ws_server_gateway.h"
#include "router/router.h"
#include "commands/command.h"
#include "config.h"
#include "config_view.h"
#include "bus/message_bus.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws_ch";

/* WebSocket Server Channel private data */
typedef struct {
    gateway_t *gateway;
    bool initialized;
    bool started;
} ws_channel_priv_t;

static ws_channel_priv_t s_priv = {0};

/**
 * Gateway message callback - routes to Input Processor
 */
static void on_gateway_message(gateway_t *gw, const char *session_id,
                                const char *content, size_t content_len, void *user_data)
{
    (void)gw;
    (void)user_data;

    /* Route through Input Processor */
    router_handle(gw, session_id, content);
}

/**
 * Initialize WebSocket Server Channel
 */
mimi_err_t ws_server_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "WebSocket Server Channel already initialized");
        return MIMI_OK;
    }

    /* Find WebSocket Gateway */
    s_priv.gateway = gateway_manager_find("websocket");
    if (!s_priv.gateway) {
        MIMI_LOGE(TAG, "WebSocket Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Register mapping for Input Processor */
    mimi_err_t err = router_register_mapping("websocket", "websocket");
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to register input processor mapping");
        return err;
    }

    /* Set up gateway callbacks */
    gateway_set_on_message(s_priv.gateway, on_gateway_message, NULL);

    /* Configure gateway port from config */
    mimi_cfg_obj_t internal = mimi_cfg_section("internal");
    int port = mimi_cfg_get_int(internal, "wsPort", 18789);
    if (port > 0) ws_gateway_configure(port, "/");

    ch->priv_data = &s_priv;
    s_priv.initialized = true;

    MIMI_LOGD(TAG, "WebSocket Server Channel initialized");
    return MIMI_OK;
}

/**
 * Start WebSocket Server Channel
 */
mimi_err_t ws_server_channel_start_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        MIMI_LOGE(TAG, "WebSocket Server Channel not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        MIMI_LOGW(TAG, "WebSocket Server Channel already started");
        return MIMI_OK;
    }

    /* Start WebSocket Gateway */
    if (s_priv.gateway && !s_priv.gateway->is_started) {
        mimi_err_t err = gateway_start(s_priv.gateway);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to start WebSocket Gateway: %s",
                      mimi_err_to_name(err));
            return err;
        }
    }

    s_priv.started = true;
    MIMI_LOGI(TAG, "WebSocket Server Channel started");
    return MIMI_OK;
}

/**
 * Stop WebSocket Server Channel
 */
mimi_err_t ws_server_channel_stop_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return MIMI_OK;
    }

    /* Stop WebSocket Gateway */
    if (s_priv.gateway && s_priv.gateway->is_started) {
        gateway_stop(s_priv.gateway);
    }

    s_priv.started = false;
    MIMI_LOGI(TAG, "WebSocket Server Channel stopped");
    return MIMI_OK;
}

/**
 * Destroy WebSocket Server Channel
 */
void ws_server_channel_destroy_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return;
    }

    ws_server_channel_stop_impl(ch);

    /* Unregister mapping */
    router_unregister_mapping("websocket");

    s_priv.gateway = NULL;
    s_priv.initialized = false;
    s_priv.started = false;

    MIMI_LOGI(TAG, "WebSocket Server Channel destroyed");
}

static mimi_err_t ws_server_channel_send_msg_impl(channel_t *ch, const mimi_msg_t *msg)
{
    (void)ch;
    if (!msg || !msg->chat_id[0] || !msg->content) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!s_priv.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.gateway) {
        return gateway_send(s_priv.gateway, msg->chat_id, msg->content);
    }

    return MIMI_ERR_INVALID_STATE;
}

/**
 * Send message through WebSocket Server Channel (legacy helper, kept for reuse)
 */
mimi_err_t ws_server_channel_send_impl(channel_t *ch, const char *session_id,
                                 const char *content)
{
    (void)ch;

    if (!s_priv.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    /* Use Gateway to send */
    if (s_priv.gateway) {
        return gateway_send(s_priv.gateway, session_id, content);
    }

    return MIMI_ERR_INVALID_STATE;
}

/**
 * Check if channel is running
 */
static bool ws_server_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.initialized && s_priv.started;
}

/**
 * Set message callback (not used - handled via Input Processor)
 */
static void ws_server_set_on_message_impl(channel_t *ch,
                                    void (*cb)(channel_t *, const char *,
                                               const char *, void *),
                                    void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
    /* WebSocket Channel uses Input Processor, not direct callbacks */
}

/**
 * Set connect callback (not used)
 */
static void ws_server_set_on_connect_impl(channel_t *ch,
                                    void (*cb)(channel_t *, const char *,
                                               void *),
                                    void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Set disconnect callback (not used)
 */
static void ws_server_set_on_disconnect_impl(channel_t *ch,
                                       void (*cb)(channel_t *, const char *,
                                                  void *),
                                       void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Initialize WebSocket Server Channel module
 */
mimi_err_t ws_server_channel_init(void)
{
    return MIMI_OK;
}

/* Global WebSocket server channel instance */
channel_t g_ws_server_channel = {
    .name = "websocket",
    .description = "WebSocket Server Channel",
    .require_auth = false,
    .max_sessions = -1,
    .init = ws_server_channel_init_impl,
    .start = ws_server_channel_start_impl,
    .stop = ws_server_channel_stop_impl,
    .destroy = ws_server_channel_destroy_impl,
    .send_msg = ws_server_channel_send_msg_impl,
    .is_running = ws_server_is_running_impl,
    .set_on_message = ws_server_set_on_message_impl,
    .set_on_connect = ws_server_set_on_connect_impl,
    .set_on_disconnect = ws_server_set_on_disconnect_impl,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false
};
