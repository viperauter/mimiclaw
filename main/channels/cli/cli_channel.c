/*
 * CLI Channel Implementation
 *
 * Adapts the CLI subsystem to the Channel interface.
 * Uses the STDIO Gateway for input/output.
 */

#include "channels/cli/cli_channel.h"
#include "channels/channel_manager.h"
#include "router/router.h"
#include "commands/command.h"
#include "config.h"
#include "bus/message_bus.h"
#include "log.h"
#include "os/os.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "gateway/gateway_manager.h"

static const char *TAG = "cli_channel";

/* CLI Channel private data */
typedef struct {
    /* Gateway reference */
    gateway_t *gateway;

    /* Session info */
    char session_id[64];
    bool has_session;
} cli_channel_priv_t;

static cli_channel_priv_t s_priv = {0};

/* Forward declarations */
static void on_gateway_message(gateway_t *gw, const char *session_id,
                                const char *content, void *user_data);

/*
 * Channel Interface Implementation
 */

static mimi_err_t cli_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    MIMI_LOGI(TAG, "Initializing CLI channel");

    memset(&s_priv, 0, sizeof(s_priv));

    /* Find STDIO Gateway */
    s_priv.gateway = gateway_manager_find("stdio");
    if (!s_priv.gateway) {
        MIMI_LOGE(TAG, "STDIO Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Set up gateway message handler */
    gateway_set_on_message(s_priv.gateway, on_gateway_message, ch);

    /* Generate session ID */
    snprintf(s_priv.session_id, sizeof(s_priv.session_id), "cli:default");
    s_priv.has_session = true;

    /* Store private data */
    ch->priv_data = &s_priv;

    MIMI_LOGI(TAG, "CLI channel initialized (session: %s)", s_priv.session_id);
    return MIMI_OK;
}

static mimi_err_t cli_channel_start_impl(channel_t *ch)
{
    cli_channel_priv_t *priv = (cli_channel_priv_t *)ch->priv_data;
    if (!priv) {
        return MIMI_ERR_INVALID_STATE;
    }

    MIMI_LOGI(TAG, "Starting CLI channel");

    /* Start the gateway */
    mimi_err_t err = gateway_start(priv->gateway);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start gateway: %d", err);
        return err;
    }

    MIMI_LOGI(TAG, "CLI channel started");
    return MIMI_OK;
}

static mimi_err_t cli_channel_stop_impl(channel_t *ch)
{
    cli_channel_priv_t *priv = (cli_channel_priv_t *)ch->priv_data;
    if (!priv) {
        return MIMI_OK;
    }

    MIMI_LOGI(TAG, "Stopping CLI channel");

    /* Stop the gateway */
    gateway_stop(priv->gateway);

    MIMI_LOGI(TAG, "CLI channel stopped");
    return MIMI_OK;
}

static void cli_channel_destroy_impl(channel_t *ch)
{
    cli_channel_priv_t *priv = (cli_channel_priv_t *)ch->priv_data;
    if (!priv) {
        return;
    }

    MIMI_LOGI(TAG, "Destroying CLI channel");

    /* Stop first */
    cli_channel_stop_impl(ch);

    /* Clear gateway callback */
    gateway_set_on_message(priv->gateway, NULL, NULL);

    memset(priv, 0, sizeof(cli_channel_priv_t));
    ch->priv_data = NULL;

    MIMI_LOGI(TAG, "CLI channel destroyed");
}

static mimi_err_t cli_channel_send_impl(channel_t *ch,
                                         const char *session_id,
                                         const char *content)
{
    cli_channel_priv_t *priv = (cli_channel_priv_t *)ch->priv_data;
    if (!priv || !priv->gateway) {
        return MIMI_ERR_INVALID_STATE;
    }

    /* Send via gateway */
    return gateway_send(priv->gateway, session_id, content);
}

static bool cli_channel_is_running_impl(channel_t *ch)
{
    cli_channel_priv_t *priv = (cli_channel_priv_t *)ch->priv_data;
    if (!priv || !priv->gateway) {
        return false;
    }

    return priv->gateway->is_started;
}

static void cli_channel_set_on_message(channel_t *ch,
                                        void (*cb)(channel_t *, const char *, 
                                                   const char *, void *),
                                        void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
    /* Messages are routed through gateway callback */
}

static void cli_channel_set_on_connect(channel_t *ch,
                                        void (*cb)(channel_t *, const char *,
                                                   void *),
                                        void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

static void cli_channel_set_on_disconnect(channel_t *ch,
                                           void (*cb)(channel_t *, const char *,
                                                      void *),
                                           void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/*
 * Gateway Message Handler
 */

static void on_gateway_message(gateway_t *gw, const char *session_id,
                                const char *content, void *user_data)
{
    (void)gw;
    channel_t *ch = (channel_t *)user_data;
    cli_channel_priv_t *priv = (cli_channel_priv_t *)ch->priv_data;

    if (!priv || !content) {
        return;
    }

    /* Route to Input Processor */
    router_handle(gw, session_id, content);
}

/*
 * Global CLI Channel Instance
 */

channel_t g_cli_channel = {
    .name = "cli",
    .description = "Command Line Interface",
    .require_auth = false,
    .max_sessions = 1,
    .init = cli_channel_init_impl,
    .start = cli_channel_start_impl,
    .stop = cli_channel_stop_impl,
    .destroy = cli_channel_destroy_impl,
    .send = cli_channel_send_impl,
    .is_running = cli_channel_is_running_impl,
    .set_on_message = cli_channel_set_on_message,
    .set_on_connect = cli_channel_set_on_connect,
    .set_on_disconnect = cli_channel_set_on_disconnect,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false
};

/*
 * Auto-initialization function for CLI Channel
 */
mimi_err_t cli_channel_init(void)
{
    return MIMI_OK;
}
