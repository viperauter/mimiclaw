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
#include "control/control_manager.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "gateway/gateway_manager.h"

/* Control message handling */
static void (*s_on_control_response)(channel_t *, const char *, const char *, const char *, void *) = NULL;
static void *s_control_response_user_data = NULL;

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
                                const char *content, size_t content_len, void *user_data);

/*
 * Channel Interface Implementation
 */

static mimi_err_t cli_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    MIMI_LOGD(TAG, "Initializing CLI channel");

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

    MIMI_LOGD(TAG, "CLI channel initialized (session: %s)", s_priv.session_id);
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

static mimi_err_t cli_channel_send_msg_impl(channel_t *ch,
                                            const mimi_msg_t *msg)
{
    const char *session_id = msg ? msg->chat_id : NULL;
    const char *content = msg ? msg->content : NULL;

    cli_channel_priv_t *priv = (cli_channel_priv_t *)ch->priv_data;
    if (!priv || !priv->gateway) {
        return MIMI_ERR_INVALID_STATE;
    }

    /* Skip formatting for status messages like "mimi is working..." */
    if (content && strstr(content, "mimi is working")) {
        return gateway_send(priv->gateway, session_id, content);
    }

    /* Format message with mimi prefix for assistant responses */
    char *formatted = NULL;
    if (content && *content) {
        size_t len = strlen(content) + 32;
        formatted = malloc(len);
        if (formatted) {
            snprintf(formatted, len, "\n🐱mimi:\n%s\n", content);
        }
    }

    /* Send via gateway */
    mimi_err_t err = gateway_send(priv->gateway, session_id, formatted ? formatted : content);

    if (formatted) {
        free(formatted);
    }

    return err;
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

static mimi_err_t cli_channel_send_control(channel_t *ch, const char *session_id,
                                         mimi_control_type_t control_type,
                                         const char *request_id,
                                         const char *target,
                                         const char *data)
{
    (void)ch;
    cli_channel_priv_t *priv = &s_priv;

    if (!priv->gateway) {
        return MIMI_ERR_INVALID_STATE;
    }

    char msg[1024];
    const char *type_str = "";

    switch (control_type) {
        case MIMI_CONTROL_TYPE_CONFIRM:
            type_str = "CONFIRM";
            break;
        case MIMI_CONTROL_TYPE_CANCEL:
            type_str = "CANCEL";
            break;
        case MIMI_CONTROL_TYPE_STOP:
            type_str = "STOP";
            break;
        case MIMI_CONTROL_TYPE_STATUS:
            type_str = "STATUS";
            break;
        default:
            type_str = "UNKNOWN";
            break;
    }

    snprintf(msg, sizeof(msg),
             "\n=== CONTROL REQUEST ====\n"
             "Type: %s\n"
             "Target: %s\n"
             "Please choose an option:\n"
             "  1. ACCEPT - Execute operation\n"
             "  2. ACCEPT_ALWAYS - Execute and always allow\n"
             "  3. REJECT - Cancel operation\n"
             ">",
             type_str, target ? target : "");

    return gateway_send(priv->gateway, session_id, msg);
}

static void cli_channel_set_on_control_response(channel_t *ch,
                                                void (*cb)(channel_t *, const char *,
                                                          const char *, const char *,
                                                          void *),
                                                void *user_data)
{
    (void)ch;
    s_on_control_response = cb;
    s_control_response_user_data = user_data;
}

/*
 * Gateway Message Handler
 */

static void on_gateway_message(gateway_t *gw, const char *session_id,
                                const char *content, size_t content_len, void *user_data)
{
    (void)gw;
    (void)content_len;
    channel_t *ch = (channel_t *)user_data;
    cli_channel_priv_t *priv = (cli_channel_priv_t *)ch->priv_data;

    if (!priv || !content) {
        return;
    }

    MIMI_LOGD(TAG, "Received content: '%s' (len=%zu)", content, strlen(content));

    /* Check if this is a control response (starts with number 1-3) */
    if (content[0] >= '1' && content[0] <= '3') {
        /* This is a control response */
        const char *response = "";
        
        switch (content[0]) {
            case '1':
                response = "ACCEPT";
                break;
            case '2':
                response = "ACCEPT_ALWAYS";
                break;
            case '3':
                response = "REJECT";
                break;
        }
        
        MIMI_LOGI(TAG, "CLI control response: %s", response);
        
        /* Handle the control response by chat ID */
        control_manager_handle_response_by_chat_id(session_id, response);
        
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
    .send_msg = cli_channel_send_msg_impl,
    .is_running = cli_channel_is_running_impl,
    .set_on_message = cli_channel_set_on_message,
    .set_on_connect = cli_channel_set_on_connect,
    .set_on_disconnect = cli_channel_set_on_disconnect,
    .send_control = cli_channel_send_control,
    .set_on_control_response = cli_channel_set_on_control_response,
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
