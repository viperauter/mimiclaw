#include "telegram/telegram_bot.h"
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

static char s_bot_token[128] = {0};
static volatile bool s_running = false;
static long long s_update_offset = 0;

static mimi_err_t tg_http_call(const char *method, const char *json_body,
                               mimi_http_response_t *out)
{
    if (!s_bot_token[0]) return MIMI_ERR_INVALID_STATE;

    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", s_bot_token, method);

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

static void handle_update_object(cJSON *upd)
{
    cJSON *update_id = cJSON_GetObjectItem(upd, "update_id");
    if (update_id && cJSON_IsNumber(update_id)) {
        long long id = (long long) update_id->valuedouble;
        if (id >= s_update_offset) s_update_offset = id + 1;
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

    mimi_msg_t m = {0};
    strncpy(m.channel, MIMI_CHAN_TELEGRAM, sizeof(m.channel) - 1);
    strncpy(m.chat_id, chat_id, sizeof(m.chat_id) - 1);
    m.content = strdup(text->valuestring);
    if (!m.content) return;

    if (message_bus_push_inbound(&m) != MIMI_OK) {
        free(m.content);
    }
}

static void telegram_poll_task(void *arg)
{
    (void) arg;
    MIMI_LOGI(TAG, "Telegram polling started");

    while (s_running) {
        /* Build getUpdates URL with offset & timeout */
        char method[128];
        if (s_update_offset > 0) {
            snprintf(method, sizeof(method),
                     "getUpdates?timeout=%d&offset=%lld",
                     TG_POLL_TIMEOUT_S, s_update_offset);
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

mimi_err_t telegram_bot_init(void)
{
    const mimi_config_t *cfg = mimi_config_get();
    if (cfg->telegram_token[0] != '\0') {
        strncpy(s_bot_token, cfg->telegram_token, sizeof(s_bot_token) - 1);
        s_bot_token[sizeof(s_bot_token) - 1] = '\0';
    }
    if (!s_bot_token[0]) {
        MIMI_LOGW(TAG, "Telegram bot token not configured (config.json channels.telegram.token)");
    } else {
        MIMI_LOGI(TAG, "Telegram bot initialized with token prefix %.6s***", s_bot_token);
    }
    return MIMI_OK;
}

mimi_err_t telegram_bot_start(void)
{
    if (!s_bot_token[0]) {
        MIMI_LOGW(TAG, "Cannot start Telegram polling without bot token");
        return MIMI_ERR_INVALID_STATE;
    }
    if (s_running) return MIMI_OK;
    s_running = true;
    return mimi_task_create_detached("telegram_poll", telegram_poll_task, NULL);
}

mimi_err_t telegram_send_message(const char *chat_id, const char *text)
{
    if (!chat_id || !text) return MIMI_ERR_INVALID_ARG;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "chat_id", chat_id);
    cJSON_AddStringToObject(body, "text", text);
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

mimi_err_t telegram_set_token(const char *token)
{
    if (!token || !token[0]) return MIMI_ERR_INVALID_ARG;
    strncpy(s_bot_token, token, sizeof(s_bot_token) - 1);
    MIMI_LOGI(TAG, "Telegram token updated (POSIX, not persisted)");
    return MIMI_OK;
}

