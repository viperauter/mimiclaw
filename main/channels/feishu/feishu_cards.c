/**
 * Feishu card rendering and message update helpers.
 *
 * - Streaming: a single interactive card updated via PATCH.
 * - Control: confirmation card with buttons, and a result card update.
 */

#include "channels/feishu/feishu_cards.h"
#include "channels/feishu/feishu_priv.h"
#include "channels/feishu/feishu_card_model.h"

#include "log.h"
#include "gateway/http/http_client_gateway.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "feishu";

static char *feishu_card_model_to_json(const feishu_card_model_t *model)
{
    cJSON *card = feishu_card_render(model);
    if (!card) return NULL;
    char *json = cJSON_PrintUnformatted(card);
    cJSON_Delete(card);
    return json;
}

/* Send a message with given msg_type and JSON content body (e.g. {"text":"..."} or interactive card JSON). */
static mimi_err_t feishu_send_raw(const char *chat_id, const char *msg_type, const char *content_json)
{
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!p || !p->tenant_access_token[0]) return MIMI_ERR_INVALID_STATE;
    if (!chat_id || !msg_type || !content_json) return MIMI_ERR_INVALID_ARG;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", msg_type);
    cJSON_AddStringToObject(body, "content", content_json);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return MIMI_ERR_NO_MEM;

    char url[512];
    snprintf(url, sizeof(url), "%s/open-apis/im/v1/messages?receive_id_type=chat_id", "https://open.feishu.cn");
    char headers[1024];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\nContent-Type: application/json\r\n",
             p->tenant_access_token);

    mimi_http_request_t req = {
        .method = "POST",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)json,
        .body_len = strlen(json),
        .timeout_ms = 30000,
    };

    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    mimi_err_t err = mimi_http_exec(&req, &resp);
    free(json);

    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to send message: %d", err);
        mimi_http_response_free(&resp);
        return err;
    }
    mimi_http_response_free(&resp);
    return MIMI_OK;
}

/* Send text message to Feishu user (chat_id for p2p). */
mimi_err_t feishu_send_text(const char *chat_id, const char *text)
{
    if (!chat_id || !text) return MIMI_ERR_INVALID_ARG;
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "text", text);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);
    if (!content_str) return MIMI_ERR_NO_MEM;
    mimi_err_t err = feishu_send_raw(chat_id, "text", content_str);
    free(content_str);
    return err;
}

mimi_err_t feishu_send_image_key(const char *chat_id, const char *image_key)
{
    if (!chat_id || !image_key) return MIMI_ERR_INVALID_ARG;
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "image_key", image_key);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);
    if (!content_str) return MIMI_ERR_NO_MEM;
    mimi_err_t err = feishu_send_raw(chat_id, "image", content_str);
    free(content_str);
    return err;
}

mimi_err_t feishu_send_audio_key(const char *chat_id, const char *file_key)
{
    if (!chat_id || !file_key) return MIMI_ERR_INVALID_ARG;
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "file_key", file_key);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);
    if (!content_str) return MIMI_ERR_NO_MEM;
    mimi_err_t err = feishu_send_raw(chat_id, "audio", content_str);
    free(content_str);
    return err;
}

static mimi_err_t feishu_send_stream_card_start(const char *chat_id,
                                                const char *content_md,
                                                char *out_msg_id,
                                                size_t out_len)
{
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!chat_id || !content_md || !out_msg_id || out_len == 0) return MIMI_ERR_INVALID_ARG;
    if (!p || !p->tenant_access_token[0]) return MIMI_ERR_INVALID_STATE;

    feishu_card_block_t blocks[] = {
        {
            .type = FEISHU_CARD_BLOCK_MARKDOWN,
            .markdown = { .md = content_md },
        },
    };
    feishu_card_model_t model = {
        .wide_screen_mode = true,
        .update_multi = true,
        .title = "Mimi",
        .subtitle = "处理中",
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
    };

    char *card_json = feishu_card_model_to_json(&model);
    if (!card_json) return MIMI_ERR_NO_MEM;

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        free(card_json);
        return MIMI_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", "interactive");
    cJSON_AddStringToObject(body, "content", card_json);
    char *body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(card_json);
    if (!body_json) return MIMI_ERR_NO_MEM;

    char url[512];
    snprintf(url, sizeof(url), "%s/open-apis/im/v1/messages?receive_id_type=chat_id", "https://open.feishu.cn");

    char headers[1024];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\nContent-Type: application/json\r\n",
             p->tenant_access_token);

    mimi_http_request_t req = {
        .method = "POST",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)body_json,
        .body_len = strlen(body_json),
        .timeout_ms = 30000,
    };

    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    mimi_err_t err = mimi_http_exec(&req, &resp);
    free(body_json);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to send Feishu stream card: %d", err);
        mimi_http_response_free(&resp);
        return err;
    }

    mimi_err_t result = MIMI_ERR_FAIL;
    if (resp.body && resp.body_len > 0) {
        cJSON *root = cJSON_Parse((const char *)resp.body);
        if (root) {
            cJSON *code = cJSON_GetObjectItem(root, "code");
            cJSON *data = cJSON_GetObjectItem(root, "data");
            cJSON *msg_id = data ? cJSON_GetObjectItem(data, "message_id") : NULL;
            if (code && cJSON_IsNumber(code) && code->valueint == 0 &&
                msg_id && cJSON_IsString(msg_id) && msg_id->valuestring) {
                strncpy(out_msg_id, msg_id->valuestring, out_len - 1);
                out_msg_id[out_len - 1] = '\0';
                result = MIMI_OK;
            }
            cJSON_Delete(root);
        }
    }

    mimi_http_response_free(&resp);
    return result;
}

static mimi_err_t feishu_update_stream_card(const char *message_id, const char *content_md)
{
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!message_id || !message_id[0] || !content_md) return MIMI_ERR_INVALID_ARG;
    if (!p || !p->tenant_access_token[0]) return MIMI_ERR_INVALID_STATE;

    feishu_card_block_t blocks[] = {
        {
            .type = FEISHU_CARD_BLOCK_MARKDOWN,
            .markdown = { .md = content_md },
        },
    };
    feishu_card_model_t model = {
        .wide_screen_mode = true,
        .update_multi = true,
        .title = "Mimi",
        .subtitle = "处理中",
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
    };

    char *card_json = feishu_card_model_to_json(&model);
    if (!card_json) return MIMI_ERR_NO_MEM;

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        free(card_json);
        return MIMI_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "msg_type", "interactive");
    cJSON_AddStringToObject(body, "content", card_json);
    char *body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(card_json);
    if (!body_json) return MIMI_ERR_NO_MEM;

    char url[512];
    snprintf(url, sizeof(url), "%s/open-apis/im/v1/messages/%s", "https://open.feishu.cn", message_id);

    char headers[1024];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\nContent-Type: application/json\r\n",
             p->tenant_access_token);

    mimi_http_request_t req = {
        .method = "PATCH",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)body_json,
        .body_len = strlen(body_json),
        .timeout_ms = 30000,
    };

    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    mimi_err_t err = mimi_http_exec(&req, &resp);
    free(body_json);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to update stream card: %d", err);
        mimi_http_response_free(&resp);
        return err;
    }

    mimi_http_response_free(&resp);
    return MIMI_OK;
}

mimi_err_t feishu_update_interactive(const char *message_id, const feishu_card_model_t *model)
{
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!message_id || !message_id[0] || !model) return MIMI_ERR_INVALID_ARG;
    if (!p || !p->tenant_access_token[0]) return MIMI_ERR_INVALID_STATE;

    char *card_json = feishu_card_model_to_json(model);
    if (!card_json) return MIMI_ERR_NO_MEM;

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        free(card_json);
        return MIMI_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "msg_type", "interactive");
    cJSON_AddStringToObject(body, "content", card_json);
    char *body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(card_json);
    if (!body_json) return MIMI_ERR_NO_MEM;

    char url[512];
    snprintf(url, sizeof(url), "%s/open-apis/im/v1/messages/%s", "https://open.feishu.cn", message_id);

    char headers[1024];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\nContent-Type: application/json\r\n",
             p->tenant_access_token);

    mimi_http_request_t req = {
        .method = "PATCH",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)body_json,
        .body_len = strlen(body_json),
        .timeout_ms = 30000,
    };

    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    mimi_err_t err = mimi_http_exec(&req, &resp);
    free(body_json);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to update interactive card: %d", err);
        mimi_http_response_free(&resp);
        return err;
    }

    mimi_http_response_free(&resp);
    return MIMI_OK;
}

mimi_err_t feishu_send_control_card(const char *chat_id,
                                    mimi_control_type_t control_type,
                                    const char *request_id,
                                    const char *target,
                                    const char *data)
{
    (void)data;
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!chat_id || !request_id) return MIMI_ERR_INVALID_ARG;
    if (!p || !p->tenant_access_token[0]) return MIMI_ERR_INVALID_STATE;

    char desc_buf[512];
    snprintf(desc_buf, sizeof(desc_buf),
             "⚠️ **需要确认的工具调用**\n\n"
             "- 工具：`%s`\n"
             "- 请求ID：`%.40s`\n\n"
             "请确认是否允许本次执行。",
             target && target[0] ? target : "(unknown)",
             request_id ? request_id : "");

    feishu_card_button_t buttons[] = {
        { .text = "允许本次", .type = "primary", .value_action = "ACCEPT",        .value_request_id = request_id, .value_target = target },
        { .text = "总是允许", .type = "primary", .value_action = "ACCEPT_ALWAYS", .value_request_id = request_id, .value_target = target },
        { .text = "拒绝",     .type = NULL,      .value_action = "REJECT",        .value_request_id = request_id, .value_target = target },
    };
    feishu_card_block_t blocks[] = {
        { .type = FEISHU_CARD_BLOCK_MARKDOWN, .markdown = { .md = desc_buf } },
        { .type = FEISHU_CARD_BLOCK_ACTIONS,  .actions  = { .buttons = buttons, .button_count = sizeof(buttons) / sizeof(buttons[0]) } },
    };
    feishu_card_model_t model = {
        .wide_screen_mode = true,
        .update_multi = true,
        .title = "操作确认",
        .subtitle = (target && target[0]) ? target : "工具调用",
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
    };

    char *card_json = feishu_card_model_to_json(&model);
    if (!card_json) return MIMI_ERR_NO_MEM;

    mimi_err_t err = feishu_send_raw(chat_id, "interactive", card_json);
    free(card_json);
    (void)control_type;
    return err;
}

mimi_err_t feishu_update_control_card_result(const char *message_id, const char *text)
{
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!message_id || !message_id[0] || !text) return MIMI_ERR_INVALID_ARG;
    if (!p || !p->tenant_access_token[0]) return MIMI_ERR_INVALID_STATE;

    feishu_card_block_t blocks[] = {
        { .type = FEISHU_CARD_BLOCK_MARKDOWN, .markdown = { .md = text } },
    };
    feishu_card_model_t model = {
        .wide_screen_mode = true,
        .update_multi = false,
        .title = "操作结果",
        .subtitle = NULL,
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
    };

    char *card_json = feishu_card_model_to_json(&model);
    if (!card_json) return MIMI_ERR_NO_MEM;

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        free(card_json);
        return MIMI_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "msg_type", "interactive");
    cJSON_AddStringToObject(body, "content", card_json);
    char *body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    free(card_json);
    if (!body_json) return MIMI_ERR_NO_MEM;

    char url[512];
    snprintf(url, sizeof(url), "%s/open-apis/im/v1/messages/%s", "https://open.feishu.cn", message_id);

    char headers[1024];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\nContent-Type: application/json\r\n",
             p->tenant_access_token);

    mimi_http_request_t req = {
        .method = "PATCH",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)body_json,
        .body_len = strlen(body_json),
        .timeout_ms = 30000,
    };

    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    mimi_err_t err = mimi_http_exec(&req, &resp);
    free(body_json);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to update control card: %d", err);
        mimi_http_response_free(&resp);
        return err;
    }

    mimi_http_response_free(&resp);
    return MIMI_OK;
}

mimi_err_t feishu_stream_start(const char *chat_id,
                               const char *content_md,
                               char *out_msg_id,
                               size_t out_len)
{
    return feishu_send_stream_card_start(chat_id, content_md, out_msg_id, out_len);
}

mimi_err_t feishu_stream_update(const char *message_id,
                                const char *content_md)
{
    return feishu_update_stream_card(message_id, content_md);
}

