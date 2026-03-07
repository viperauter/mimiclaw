/**
 * Telegram Channel Implementation
 *
 * Full implementation of Telegram Bot as a Channel.
 * No backward compatibility - all logic is self-contained.
 */

#include "channels/telegram/telegram_channel.h"
#include "channels/channel_manager.h"
#include "commands/command.h"
#include "config.h"
#include "bus/message_bus.h"
#include "http/http.h"
#include "log.h"
#include "os/os.h"

#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "telegram";
static const int TG_POLL_TIMEOUT_S = 30;

/* Telegram Channel private data */
typedef struct {
    bool initialized;
    bool started;
    
    char bot_token[128];
    volatile bool running;
    long long update_offset;
    
    /* Callbacks */
    void (*on_message)(channel_t *, const char *, const char *, void *);
    void (*on_connect)(channel_t *, const char *, void *);
    void (*on_disconnect)(channel_t *, const char *, void *);
    void *callback_user_data;
} telegram_channel_priv_t;

static telegram_channel_priv_t s_priv = {0};

/* Forward declarations */
static void telegram_set_on_message_impl(channel_t *ch,
                                          void (*cb)(channel_t *, const char *,
                                                     const char *, void *),
                                          void *user_data);
static void telegram_set_on_connect_impl(channel_t *ch,
                                          void (*cb)(channel_t *, const char *,
                                                     void *),
                                          void *user_data);
static void telegram_set_on_disconnect_impl(channel_t *ch,
                                             void (*cb)(channel_t *, const char *,
                                                        void *),
                                             void *user_data);
static bool telegram_is_running_impl(channel_t *ch);

/**
 * HTTP call to Telegram API
 */
static mimi_err_t tg_http_call(const char *method, const char *json_body,
                               mimi_http_response_t *out)
{
    if (!s_priv.bot_token[0]) return MIMI_ERR_INVALID_STATE;

    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", s_priv.bot_token, method);

    char headers[128] =
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n";

    mimi_http_request_t req = {
        .method = json_body ? "POST" : "GET",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *) json_body,
        .body_len = json_body ? strlen(json_body) : 0,
        .timeout_ms = (TG_POLL_TIMEOUT_S + 5) * 1000,
    };

    return mimi_http_exec(&req, out);
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

    /* Call channel callback if set */
    if (s_priv.on_message) {
        channel_t *ch = &g_telegram_channel;
        s_priv.on_message(ch, chat_id, text->valuestring, s_priv.callback_user_data);
    }

    /* Also push to message bus for backward compatibility during transition */
    mimi_msg_t m = {0};
    strncpy(m.channel, MIMI_CHAN_TELEGRAM, sizeof(m.channel) - 1);
    strncpy(m.chat_id, chat_id, sizeof(m.chat_id) - 1);
    m.content = strdup(text->valuestring);
    if (!m.content) return;

    if (message_bus_push_inbound(&m) != MIMI_OK) {
        free(m.content);
    }
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

        mimi_http_response_t resp;
        mimi_err_t err = tg_http_call(method, NULL, &resp);
        if (err != MIMI_OK) {
            MIMI_LOGW(TAG, "getUpdates failed: %s", mimi_err_to_name(err));
            mimi_http_response_free(&resp);
            continue;
        }

        if (resp.status != 200 || !resp.body) {
            MIMI_LOGW(TAG, "getUpdates HTTP %d", resp.status);
            mimi_http_response_free(&resp);
            continue;
        }

        cJSON *root = cJSON_Parse((char *) resp.body);
        mimi_http_response_free(&resp);
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

    /* Load token from config */
    const mimi_config_t *config = mimi_config_get();
    if (config->telegram_token[0] != '\0') {
        strncpy(s_priv.bot_token, config->telegram_token, sizeof(s_priv.bot_token) - 1);
        s_priv.bot_token[sizeof(s_priv.bot_token) - 1] = '\0';
    }

    if (!s_priv.bot_token[0]) {
        MIMI_LOGW(TAG, "Telegram bot token not configured (config.json channels.telegram.token)");
    } else {
        MIMI_LOGI(TAG, "Telegram bot initialized with token prefix %.6s***", s_priv.bot_token);
    }

    /* Store channel reference */
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

    if (!s_priv.bot_token[0]) {
        MIMI_LOGW(TAG, "Cannot start Telegram polling without bot token");
        return MIMI_ERR_INVALID_STATE;
    }

    /* Start polling task */
    s_priv.running = true;
    mimi_err_t err = mimi_task_create_detached("telegram_poll", telegram_poll_task, NULL);
    if (err != MIMI_OK) {
        s_priv.running = false;
        MIMI_LOGE(TAG, "Failed to create telegram poll task: %s", mimi_err_to_name(err));
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

    /* Stop first */
    telegram_channel_stop_impl(ch);

    /* Clean up state */
    memset(&s_priv, 0, sizeof(s_priv));

    MIMI_LOGI(TAG, "Telegram Channel destroyed");
}

/**
 * Send message through Telegram Channel
 */
mimi_err_t telegram_channel_send_impl(channel_t *ch, const char *session_id,
                                       const char *content)
{
    (void)ch;

    if (!s_priv.initialized || !s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    /* Build and send message */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "chat_id", session_id);
    cJSON_AddStringToObject(body, "text", content);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return MIMI_ERR_NO_MEM;

    mimi_http_response_t resp;
    mimi_err_t err = tg_http_call("sendMessage", json, &resp);
    free(json);
    if (err != MIMI_OK) return err;

    if (resp.status != 200) {
        MIMI_LOGW(TAG, "sendMessage HTTP %d", resp.status);
        mimi_http_response_free(&resp);
        return MIMI_ERR_FAIL;
    }

    mimi_http_response_free(&resp);
    return MIMI_OK;
}

/**
 * Check if channel is running
 */
static bool telegram_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.initialized && s_priv.started;
}

/**
 * Set message callback
 */
static void telegram_set_on_message_impl(channel_t *ch,
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
static void telegram_set_on_connect_impl(channel_t *ch,
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
static void telegram_set_on_disconnect_impl(channel_t *ch,
                                             void (*cb)(channel_t *, const char *,
                                                        void *),
                                             void *user_data)
{
    (void)ch;
    s_priv.on_disconnect = cb;
    s_priv.callback_user_data = user_data;
}

/**
 * Initialize Telegram Channel module
 * Called before registering with Channel Manager
 */
mimi_err_t telegram_channel_init(void)
{
    /* Initialize the global channel structure */
    strncpy(g_telegram_channel.name, "telegram", sizeof(g_telegram_channel.name) - 1);
    g_telegram_channel.name[sizeof(g_telegram_channel.name) - 1] = '\0';
    
    strncpy(g_telegram_channel.description, "Telegram Bot Channel", 
            sizeof(g_telegram_channel.description) - 1);
    g_telegram_channel.description[sizeof(g_telegram_channel.description) - 1] = '\0';
    
    g_telegram_channel.require_auth = false;
    g_telegram_channel.max_sessions = -1;
    g_telegram_channel.init = telegram_channel_init_impl;
    g_telegram_channel.start = telegram_channel_start_impl;
    g_telegram_channel.stop = telegram_channel_stop_impl;
    g_telegram_channel.destroy = telegram_channel_destroy_impl;
    g_telegram_channel.send = telegram_channel_send_impl;
    g_telegram_channel.is_running = telegram_is_running_impl;
    g_telegram_channel.set_on_message = telegram_set_on_message_impl;
    g_telegram_channel.set_on_connect = telegram_set_on_connect_impl;
    g_telegram_channel.set_on_disconnect = telegram_set_on_disconnect_impl;
    g_telegram_channel.priv_data = NULL;
    g_telegram_channel.is_initialized = false;
    g_telegram_channel.is_started = false;

    return MIMI_OK;
}

/**
 * Set Telegram bot token
 */
mimi_err_t telegram_channel_set_token(const char *token)
{
    if (!token || !token[0]) return MIMI_ERR_INVALID_ARG;
    strncpy(s_priv.bot_token, token, sizeof(s_priv.bot_token) - 1);
    MIMI_LOGI(TAG, "Telegram token updated");
    return MIMI_OK;
}

/* Global Telegram channel instance */
channel_t g_telegram_channel = {
    .name = "telegram",
    .description = "Telegram Bot Channel",
    .require_auth = false,
    .max_sessions = -1,
    .init = telegram_channel_init_impl,
    .start = telegram_channel_start_impl,
    .stop = telegram_channel_stop_impl,
    .destroy = telegram_channel_destroy_impl,
    .send = telegram_channel_send_impl,
    .is_running = telegram_is_running_impl,
    .set_on_message = telegram_set_on_message_impl,
    .set_on_connect = telegram_set_on_connect_impl,
    .set_on_disconnect = telegram_set_on_disconnect_impl,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false,
};
