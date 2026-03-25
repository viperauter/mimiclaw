/**
 * Telegram Channel Implementation
 *
 * Uses HTTP Gateway for Telegram Bot API integration.
 * Routes messages through Input Processor for command/chat routing.
 */

#include "channels/telegram/telegram_channel.h"
#include "channels/channel_manager.h"
#include "router/router.h"
#include "commands/command.h"
#include "config.h"
#include "config_view.h"
#include "bus/message_bus.h"
#include "gateway/http/http_client_gateway.h"
#include "gateway/gateway_manager.h"
#include "log.h"
#include "os/os.h"

#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "telegram";
static const int TG_POLL_TIMEOUT_S = 30;
static const char *TELEGRAM_API_BASE = "https://api.telegram.org/bot";

/* Telegram Channel private data */
typedef struct {
    bool initialized;
    bool started;
    gateway_t *http_gateway;
    char bot_token[128];
    volatile bool running;
    long long update_offset;
} telegram_channel_priv_t;

static telegram_channel_priv_t s_priv = {0};

/* Forward declarations */
static bool telegram_is_running_impl(channel_t *ch);

/**
 * HTTP call to Telegram API using HTTP Gateway
 */
static mimi_err_t tg_http_call(const char *method, const char *json_body,
                               char *response, size_t response_len)
{
    if (!s_priv.http_gateway) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    /* Build base URL with bot token */
    char base_url[256];
    snprintf(base_url, sizeof(base_url), "%s%s/", TELEGRAM_API_BASE, s_priv.bot_token);
    
    http_request_options_t opts = {
        .base_url = base_url,
        .auth_token = NULL,  /* Telegram uses token in URL, not header */
        .extra_headers = NULL,
        .timeout_ms = 35000
    };
    
    if (json_body) {
        return http_client_gateway_post(s_priv.http_gateway, method, &opts,
                                        json_body, strlen(json_body), 
                                        response, response_len);
    } else {
        return http_client_gateway_get(s_priv.http_gateway, method, &opts,
                                       response, response_len);
    }
}

/**
 * Handle incoming update from Telegram
 */
static void handle_update_object(cJSON *upd)
{
    cJSON *update_id = cJSON_GetObjectItem(upd, "update_id");
    if (update_id && cJSON_IsNumber(update_id)) {
        long long id = (long long) update_id->valuedouble;
        if (id >= s_priv.update_offset) s_priv.update_offset = id + 1;
    }

    cJSON *msg = cJSON_GetObjectItem(upd, "message");
    if (!msg) msg = cJSON_GetObjectItem(upd, "edited_message");
    if (!msg) return;

    cJSON *chat = cJSON_GetObjectItem(msg, "chat");
    cJSON *text = cJSON_GetObjectItem(msg, "text");
    if (!chat || !text || !cJSON_IsString(text)) return;

    cJSON *chat_id_j = cJSON_GetObjectItem(chat, "id");
    if (!chat_id_j || !cJSON_IsNumber(chat_id_j)) return;

    char chat_id[32];
    snprintf(chat_id, sizeof(chat_id), "%lld", (long long) chat_id_j->valuedouble);

    MIMI_LOGI(TAG, "Incoming message from chat %s: %.40s...", chat_id, text->valuestring);

    /* Route through Input Processor - generic handler eliminates per-channel boilerplate */
    router_handle_generic("telegram", chat_id, text->valuestring);
}

/**
 * Telegram polling task
 */
static void telegram_poll_task(void *arg)
{
    (void) arg;
    MIMI_LOGI(TAG, "Telegram polling started");

    while (s_priv.running) {
        /* Build getUpdates URL with offset & timeout */
        char method[128];
        if (s_priv.update_offset > 0) {
            snprintf(method, sizeof(method),
                     "getUpdates?timeout=%d&offset=%lld",
                     TG_POLL_TIMEOUT_S, s_priv.update_offset);
        } else {
            snprintf(method, sizeof(method),
                     "getUpdates?timeout=%d",
                     TG_POLL_TIMEOUT_S);
        }

        char response[8192];
        mimi_err_t err = tg_http_call(method, NULL, response, sizeof(response));
        if (err != MIMI_OK) {
            MIMI_LOGW(TAG, "getUpdates failed: %d", err);
            continue;
        }

        if (response[0] == '\0') {
            MIMI_LOGW(TAG, "getUpdates: empty response");
            continue;
        }

        cJSON *root = cJSON_Parse(response);
        if (!root) {
            MIMI_LOGW(TAG, "getUpdates: invalid JSON");
            continue;
        }

        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *result = cJSON_GetObjectItem(root, "result");
        if (ok && cJSON_IsTrue(ok) && result && cJSON_IsArray(result)) {
            cJSON *upd;
            cJSON_ArrayForEach(upd, result) {
                handle_update_object(upd);
            }
        }

        cJSON_Delete(root);
    }

    MIMI_LOGI(TAG, "Telegram polling stopped");
}

/**
 * Initialize Telegram Channel
 */
mimi_err_t telegram_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "Telegram Channel already initialized");
        return MIMI_OK;
    }

    /* Check if Telegram is enabled */
    mimi_cfg_obj_t tg = mimi_cfg_named("channels", "telegram");
    if (!mimi_cfg_get_bool(tg, "enabled", false)) {
        MIMI_LOGD(TAG, "Telegram Channel is disabled");
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Load token from config */
    
    const char *token = mimi_cfg_get_str(tg, "token", "");
    if (token && token[0] != '\0') {
        strncpy(s_priv.bot_token, token, sizeof(s_priv.bot_token) - 1);
        s_priv.bot_token[sizeof(s_priv.bot_token) - 1] = '\0';
    }

    if (!s_priv.bot_token[0]) {
        MIMI_LOGW(TAG, "Telegram bot token not configured (config.json channels.telegram.token)");
    } else {
        MIMI_LOGI(TAG, "Telegram bot initialized with token prefix %.6s***", s_priv.bot_token);
    }

    /* Get HTTP Gateway */
    s_priv.http_gateway = gateway_manager_find("http");
    if (!s_priv.http_gateway) {
        MIMI_LOGE(TAG, "HTTP Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Register mapping for Input Processor */
    mimi_err_t err = router_register_mapping("telegram", "telegram");
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to register input processor mapping");
        return err;
    }

    ch->priv_data = &s_priv;
    s_priv.initialized = true;

    MIMI_LOGI(TAG, "Telegram Channel initialized");
    return MIMI_OK;
}

/**
 * Start Telegram Channel
 */
mimi_err_t telegram_channel_start_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        MIMI_LOGE(TAG, "Telegram Channel not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        MIMI_LOGW(TAG, "Telegram Channel already started");
        return MIMI_OK;
    }

    if (!s_priv.http_gateway) {
        MIMI_LOGW(TAG, "Cannot start Telegram without HTTP Gateway");
        return MIMI_ERR_INVALID_STATE;
    }

    /* Start HTTP Gateway */
    mimi_err_t err = gateway_start(s_priv.http_gateway);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start HTTP Gateway: %d", err);
        return err;
    }

    /* Start polling task */
    s_priv.running = true;
    err = mimi_task_create_detached("telegram_poll", telegram_poll_task, NULL);
    if (err != MIMI_OK) {
        s_priv.running = false;
        MIMI_LOGE(TAG, "Failed to create telegram poll task: %d", err);
        gateway_stop(s_priv.http_gateway);
        return err;
    }

    s_priv.started = true;
    MIMI_LOGI(TAG, "Telegram Channel started");
    return MIMI_OK;
}

/**
 * Stop Telegram Channel
 */
mimi_err_t telegram_channel_stop_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return MIMI_OK;
    }

    s_priv.running = false;
    s_priv.started = false;

    /* Stop HTTP Gateway */
    if (s_priv.http_gateway) {
        gateway_stop(s_priv.http_gateway);
    }

    MIMI_LOGI(TAG, "Telegram Channel stopped");
    return MIMI_OK;
}

/**
 * Destroy Telegram Channel
 */
void telegram_channel_destroy_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return;
    }

    telegram_channel_stop_impl(ch);

    /* Unregister mapping */
    router_unregister_mapping("telegram");

    memset(&s_priv, 0, sizeof(s_priv));

    MIMI_LOGI(TAG, "Telegram Channel destroyed");
}

/**
 * Check if Telegram Channel is running
 */
static bool telegram_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.running;
}

/**
 * Set message callback (not used - polling is used)
 */
static void telegram_set_on_message(channel_t *ch,
                                void (*cb)(channel_t *, const char *, 
                                           const char *, void *),
                                void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
    /* Messages are handled via polling */
}

/**
 * Set connect callback (not used)
 */
static void telegram_set_on_connect(channel_t *ch,
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
static void telegram_set_on_disconnect(channel_t *ch,
                                   void (*cb)(channel_t *, const char *, 
                                              void *),
                                   void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

static mimi_err_t telegram_channel_send_msg_impl(channel_t *ch, const mimi_msg_t *msg)
{
    (void)ch;

    if (!s_priv.initialized || !s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!msg || !msg->chat_id[0]) {
        return MIMI_ERR_INVALID_ARG;
    }

    const char *content = msg->content ? msg->content : "";

    /* Build and send message */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "chat_id", msg->chat_id);
    cJSON_AddStringToObject(body, "text", content);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return MIMI_ERR_NO_MEM;

    char response[8192];
    mimi_err_t err = tg_http_call("sendMessage", json, response, sizeof(response));
    free(json);
    
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to send message: %d", err);
        return err;
    }

    return MIMI_OK;
}

/**
 * Global Telegram Channel instance
 */
channel_t g_telegram_channel = {
    .name = "telegram",
    .description = "Telegram Bot",
    .require_auth = false,
    .max_sessions = 0,
    .init = telegram_channel_init_impl,
    .start = telegram_channel_start_impl,
    .stop = telegram_channel_stop_impl,
    .destroy = telegram_channel_destroy_impl,
    .send_msg = telegram_channel_send_msg_impl,
    .is_running = telegram_is_running_impl,
    .set_on_message = telegram_set_on_message,
    .set_on_connect = telegram_set_on_connect,
    .set_on_disconnect = telegram_set_on_disconnect,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false
};

/**
 * Initialize Telegram Channel module
 */
mimi_err_t telegram_channel_init(void)
{
    return MIMI_OK;
}

/**
 * Set Telegram bot token
 */
mimi_err_t telegram_channel_set_token(const char *token)
{
    if (!token) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    strncpy(s_priv.bot_token, token, sizeof(s_priv.bot_token) - 1);
    s_priv.bot_token[sizeof(s_priv.bot_token) - 1] = '\0';
    
    /* Token will be used in each request via options, no gateway reconfigure needed */
    
    MIMI_LOGI(TAG, "Telegram bot token updated");
    return MIMI_OK;
}
