/**
 * Feishu (Lark) Channel Implementation
 *
 * Uses WebSocket Client Gateway for Feishu Bot integration.
 * Provides real-time message handling via WebSocket connection.
 */

#include "channels/feishu/feishu_channel.h"
#include "channels/feishu/feishu_media.h"
#include "channels/channel_manager.h"
#include "router/router.h"
#include "commands/command.h"
#include "config.h"
#include "bus/message_bus.h"
#include "gateway/websocket/ws_client_gateway.h"
#include "gateway/http/http_client_gateway.h"
#include "gateway/gateway_manager.h"
#include "log.h"
#include "os/os.h"
#include "fs/fs.h"

#include "cJSON.h"

#include "channels/feishu/pb/pbbp2.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

static const char *TAG = "feishu";

/* Feishu Channel private data */
typedef struct {
    bool initialized;
    bool started;
    gateway_t *ws_gateway;
    gateway_t *http_gateway;
    char app_id[64];
    char app_secret[128];
    char tenant_access_token[512];
    char ws_url[256];
    const char *ws_token; /* points to tenant_access_token or NULL */
    volatile bool running;
    volatile bool stopping;

    /* Startup state machine */
    enum {
        FEISHU_SM_IDLE = 0,
        FEISHU_SM_GET_TENANT_TOKEN,
        FEISHU_SM_GET_WS_URL,
        FEISHU_SM_START_WS,
        FEISHU_SM_RUNNING,
        FEISHU_SM_FAILED,
    } sm_state;
} feishu_channel_priv_t;

static feishu_channel_priv_t s_priv = {0};

/* Forward declarations */
static bool feishu_is_running_impl(channel_t *ch);

typedef void (*feishu_http_async_cb)(mimi_err_t err, mimi_http_response_t *resp, void *user_data);
static mimi_err_t feishu_http_post_json_async(const char *path, const char *json_body,
                                             feishu_http_async_cb cb, void *user_data);
static void feishu_sm_start(channel_t *ch);
static void feishu_sm_fail(mimi_err_t err, const char *why);
static void feishu_sm_on_tenant_token(mimi_err_t err, mimi_http_response_t *resp, void *user_data);
static void feishu_sm_on_ws_url(mimi_err_t err, mimi_http_response_t *resp, void *user_data);

static mimi_err_t feishu_http_post_json_async(const char *path, const char *json_body,
                                             feishu_http_async_cb cb, void *user_data)
{
    if (!path || !path[0] || !cb) return MIMI_ERR_INVALID_ARG;

    char url[512];
    snprintf(url, sizeof(url), "%s%s", "https://open.feishu.cn", path);

    const char *headers = "Content-Type: application/json\r\n";
    mimi_http_request_t req = {
        .method = "POST",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)json_body,
        .body_len = json_body ? strlen(json_body) : 0,
        .timeout_ms = 30000,
    };

    mimi_http_response_t *resp = (mimi_http_response_t *)calloc(1, sizeof(*resp));
    if (!resp) return MIMI_ERR_NO_MEM;

    mimi_err_t err = mimi_http_exec_async(&req, resp, cb, user_data);
    if (err != MIMI_OK) {
        free(resp);
        return err;
    }
    return MIMI_OK;
}

static void feishu_sm_fail(mimi_err_t err, const char *why)
{
    if (s_priv.stopping) return;
    s_priv.sm_state = FEISHU_SM_FAILED;
    MIMI_LOGE(TAG, "Feishu startup failed (%s): %d", why ? why : "unknown", err);
}

static void feishu_sm_on_tenant_token(mimi_err_t err, mimi_http_response_t *resp, void *user_data)
{
    (void)user_data;

    if (s_priv.stopping) {
        if (resp) { mimi_http_response_free(resp); free(resp); }
        return;
    }

    if (err != MIMI_OK || !resp || !resp->body || resp->body_len == 0) {
        if (resp) { mimi_http_response_free(resp); free(resp); }
        feishu_sm_fail(err != MIMI_OK ? err : MIMI_ERR_FAIL, "tenant_token");
        /* still allow fallback ws url without token */
        s_priv.tenant_access_token[0] = '\0';
    } else {
        cJSON *root = cJSON_Parse((const char *)resp->body);
        if (!root) {
            feishu_sm_fail(MIMI_ERR_FAIL, "tenant_token_parse");
        } else {
            cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
            if (token && cJSON_IsString(token)) {
                strncpy(s_priv.tenant_access_token, token->valuestring,
                        sizeof(s_priv.tenant_access_token) - 1);
                s_priv.tenant_access_token[sizeof(s_priv.tenant_access_token) - 1] = '\0';
                MIMI_LOGI(TAG, "Tenant token acquired");
            } else {
                feishu_sm_fail(MIMI_ERR_FAIL, "tenant_token_missing");
            }
            cJSON_Delete(root);
        }
    }

    if (resp) { mimi_http_response_free(resp); free(resp); }

    /* Next: request WS URL (official endpoint). */
    s_priv.sm_state = FEISHU_SM_GET_WS_URL;
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        feishu_sm_fail(MIMI_ERR_NO_MEM, "ws_url_body");
        return;
    }
    cJSON_AddStringToObject(body, "AppID", s_priv.app_id);
    cJSON_AddStringToObject(body, "AppSecret", s_priv.app_secret);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        feishu_sm_fail(MIMI_ERR_NO_MEM, "ws_url_json");
        return;
    }

    mimi_err_t e2 = feishu_http_post_json_async("/callback/ws/endpoint", json, feishu_sm_on_ws_url, NULL);
    free(json);
    if (e2 != MIMI_OK) {
        feishu_sm_fail(e2, "ws_url_request");
    }
}

static void feishu_sm_on_ws_url(mimi_err_t err, mimi_http_response_t *resp, void *user_data)
{
    (void)user_data;

    if (s_priv.stopping) {
        if (resp) { mimi_http_response_free(resp); free(resp); }
        return;
    }

    s_priv.ws_url[0] = '\0';
    s_priv.ws_token = NULL;

    bool ok = (err == MIMI_OK && resp && resp->body && resp->body_len > 0);
    if (ok) {
        cJSON *root = cJSON_Parse((const char *)resp->body);
        if (root) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            cJSON *url = data ? cJSON_GetObjectItem(data, "URL") : NULL;
            if (url && cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
                strncpy(s_priv.ws_url, url->valuestring, sizeof(s_priv.ws_url) - 1);
                s_priv.ws_url[sizeof(s_priv.ws_url) - 1] = '\0';
                s_priv.ws_token = NULL; /* auth embedded */
                ok = true;
            } else {
                ok = false;
            }
            cJSON_Delete(root);
        } else {
            ok = false;
        }
    }

    if (!ok) {
        MIMI_LOGW(TAG, "Feishu endpoints not available, falling back to hyper-event URL");
        snprintf(s_priv.ws_url, sizeof(s_priv.ws_url),
                 "wss://open.feishu.cn/open-apis/bot/v3/hyper-event?app_id=%s",
                 s_priv.app_id);
        s_priv.ws_token = (s_priv.tenant_access_token[0] != '\0') ? s_priv.tenant_access_token : NULL;
    }

    if (resp) { mimi_http_response_free(resp); free(resp); }

    /* Configure + start WS gateway (non-blocking). */
    s_priv.sm_state = FEISHU_SM_START_WS;
    mimi_err_t cfg_err = ws_client_gateway_configure(s_priv.ws_url, s_priv.ws_token, 30000, 30000);
    if (cfg_err != MIMI_OK) {
        feishu_sm_fail(cfg_err, "ws_configure");
        return;
    }

    mimi_err_t start_err = gateway_start(s_priv.ws_gateway);
    if (start_err != MIMI_OK) {
        feishu_sm_fail(start_err, "ws_start");
        return;
    }

    s_priv.running = true;
    s_priv.started = true;
    s_priv.sm_state = FEISHU_SM_RUNNING;
    MIMI_LOGI(TAG, "Feishu Channel started (async state machine)");
}

static void feishu_sm_start(channel_t *ch)
{
    (void)ch;

    if (s_priv.stopping) return;
    if (!s_priv.app_id[0] || !s_priv.app_secret[0]) {
        feishu_sm_fail(MIMI_ERR_INVALID_STATE, "credentials");
        return;
    }

    s_priv.sm_state = FEISHU_SM_GET_TENANT_TOKEN;

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        feishu_sm_fail(MIMI_ERR_NO_MEM, "tenant_body");
        return;
    }
    cJSON_AddStringToObject(body, "app_id", s_priv.app_id);
    cJSON_AddStringToObject(body, "app_secret", s_priv.app_secret);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        feishu_sm_fail(MIMI_ERR_NO_MEM, "tenant_json");
        return;
    }

    mimi_err_t e = feishu_http_post_json_async("/open-apis/auth/v3/tenant_access_token/internal",
                                              json, feishu_sm_on_tenant_token, NULL);
    free(json);
    if (e != MIMI_OK) {
        feishu_sm_fail(e, "tenant_request");
        return;
    }
}

/* Send a message with given msg_type and JSON content body (e.g. {"text":"..."}, {"image_key":"..."}, {"file_key":"..."}). */
static mimi_err_t feishu_send_raw(const char *user_id, const char *msg_type, const char *content_json)
{
    if (!s_priv.tenant_access_token[0]) return MIMI_ERR_INVALID_STATE;
    if (!user_id || !msg_type || !content_json) return MIMI_ERR_INVALID_ARG;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "receive_id", user_id);
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
             s_priv.tenant_access_token);

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

/**
 * Send text message to Feishu user (chat_id for p2p).
 */
static mimi_err_t feishu_send_message(const char *user_id, const char *content)
{
    if (!user_id || !content) return MIMI_ERR_INVALID_ARG;
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "text", content);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);
    if (!content_str) return MIMI_ERR_NO_MEM;
    mimi_err_t err = feishu_send_raw(user_id, "text", content_str);
    free(content_str);
    return err;
}

/** Send image message using already-uploaded image_key. */
static mimi_err_t feishu_send_image(const char *user_id, const char *image_key)
{
    if (!user_id || !image_key) return MIMI_ERR_INVALID_ARG;
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "image_key", image_key);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);
    if (!content_str) return MIMI_ERR_NO_MEM;
    mimi_err_t err = feishu_send_raw(user_id, "image", content_str);
    free(content_str);
    return err;
}

/** Send audio/voice message using already-uploaded file_key. */
static mimi_err_t feishu_send_audio(const char *user_id, const char *file_key)
{
    if (!user_id || !file_key) return MIMI_ERR_INVALID_ARG;
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "file_key", file_key);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);
    if (!content_str) return MIMI_ERR_NO_MEM;
    mimi_err_t err = feishu_send_raw(user_id, "audio", content_str);
    free(content_str);
    return err;
}

/**
 * Handle incoming message from Feishu
 */
static void handle_message(const char *user_id, const char *content)
{
    MIMI_LOGI(TAG, "Incoming message from %s: %.40s...", user_id, content);

#ifdef FEISHU_TEST_REPLY
    /* Optional: send a simple echo reply for debugging */
    MIMI_LOGI(TAG, "Sending test reply to user %s", user_id);
    mimi_err_t err = feishu_send_message(user_id, "收到你的消息: ");
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to send test reply: %d", err);
    } else {
        MIMI_LOGI(TAG, "Test reply sent successfully");
    }
#endif

    /* Route through Input Processor */
    router_handle_feishu(user_id, content);
}

typedef struct {
    char *buf;
    size_t capacity;
    size_t len;
} feishu_payload_buf_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
} feishu_bytes_t;

typedef struct {
    char key[64];
    char value[256];
} feishu_hdr_kv_t;

typedef struct {
    /* Extracted well-known fields (also stored in kvs) */
    char type[16];        /* "event", "card", "ping", "pong" */
    char message_id[128];
    char trace_id[128];
    int sum;
    int seq;

    /* Full header set for ACK echo */
    feishu_hdr_kv_t kvs[48];
    size_t kv_count;
} feishu_frame_meta_t;

static bool feishu_payload_encode_cb(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    const feishu_bytes_t *src = (const feishu_bytes_t *)(*arg);
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_string(stream, src->buf, src->len);
}

static bool feishu_str_decode_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    (void)field;
    feishu_payload_buf_t *dst = (feishu_payload_buf_t *)(*arg);
    size_t to_read = stream->bytes_left;
    if (to_read > dst->capacity - 1) {
        to_read = dst->capacity - 1;
    }
    if (!pb_read(stream, (pb_byte_t *)dst->buf, to_read)) {
        return false;
    }
    dst->len = to_read;
    dst->buf[to_read] = '\0';
    return true;
}

static bool feishu_headers_decode_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    (void)field;
    feishu_frame_meta_t *out = (feishu_frame_meta_t *)(*arg);
    if (!out) return false;

    char key_buf[64] = {0};
    feishu_payload_buf_t key = {.buf = key_buf, .capacity = sizeof(key_buf), .len = 0};
    char val_buf[256] = {0};
    feishu_payload_buf_t val = {.buf = val_buf, .capacity = sizeof(val_buf), .len = 0};

    pbbp2_Header h = pbbp2_Header_init_zero;
    h.key.funcs.decode = feishu_str_decode_cb;
    h.key.arg = &key;
    h.value.funcs.decode = feishu_str_decode_cb;
    h.value.arg = &val;

    if (!pb_decode(stream, pbbp2_Header_fields, &h)) {
        return false;
    }

    /* Save full header list for ACK echo */
    if (out->kv_count < (sizeof(out->kvs) / sizeof(out->kvs[0]))) {
        feishu_hdr_kv_t *kv = &out->kvs[out->kv_count++];
        strncpy(kv->key, key_buf, sizeof(kv->key) - 1);
        kv->key[sizeof(kv->key) - 1] = '\0';
        strncpy(kv->value, val_buf, sizeof(kv->value) - 1);
        kv->value[sizeof(kv->value) - 1] = '\0';
    }

    if (strcmp(key_buf, "type") == 0) {
        strncpy(out->type, val_buf, sizeof(out->type) - 1);
        out->type[sizeof(out->type) - 1] = '\0';
    } else if (strcmp(key_buf, "message_id") == 0) {
        strncpy(out->message_id, val_buf, sizeof(out->message_id) - 1);
        out->message_id[sizeof(out->message_id) - 1] = '\0';
    } else if (strcmp(key_buf, "trace_id") == 0) {
        strncpy(out->trace_id, val_buf, sizeof(out->trace_id) - 1);
        out->trace_id[sizeof(out->trace_id) - 1] = '\0';
    } else if (strcmp(key_buf, "sum") == 0) {
        out->sum = atoi(val_buf);
    } else if (strcmp(key_buf, "seq") == 0) {
        out->seq = atoi(val_buf);
    }

    return true;
}

static bool feishu_headers_encode_cb(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    const feishu_frame_meta_t *m = (const feishu_frame_meta_t *)(*arg);
    if (!m) return true;

    for (size_t i = 0; i < m->kv_count; i++) {
        const feishu_hdr_kv_t *kv = &m->kvs[i];
        if (!kv->key[0]) continue;

        pbbp2_Header sub = pbbp2_Header_init_zero;
        feishu_bytes_t kb = {.buf = (const uint8_t *)kv->key, .len = strlen(kv->key)};
        feishu_bytes_t vb = {.buf = (const uint8_t *)kv->value, .len = strlen(kv->value)};
        sub.key.funcs.encode = feishu_payload_encode_cb;
        sub.key.arg = &kb;
        sub.value.funcs.encode = feishu_payload_encode_cb;
        sub.value.arg = &vb;

        if (!pb_encode_tag_for_field(stream, field)) return false;
        if (!pb_encode_submessage(stream, pbbp2_Header_fields, &sub)) return false;
    }

    /* Append biz_rt if missing */
    bool has_biz_rt = false;
    for (size_t i = 0; i < m->kv_count; i++) {
        if (strcmp(m->kvs[i].key, "biz_rt") == 0) { has_biz_rt = true; break; }
    }
    if (!has_biz_rt) {
        pbbp2_Header sub = pbbp2_Header_init_zero;
        feishu_bytes_t kb = {.buf = (const uint8_t *)"biz_rt", .len = 6};
        feishu_bytes_t vb = {.buf = (const uint8_t *)"0", .len = 1};
        sub.key.funcs.encode = feishu_payload_encode_cb; sub.key.arg = &kb;
        sub.value.funcs.encode = feishu_payload_encode_cb; sub.value.arg = &vb;
        if (!pb_encode_tag_for_field(stream, field)) return false;
        if (!pb_encode_submessage(stream, pbbp2_Header_fields, &sub)) return false;
    }

    return true;
}

static bool feishu_payload_decode_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    (void)field;
    feishu_payload_buf_t *dst = (feishu_payload_buf_t *)(*arg);
    size_t to_read = stream->bytes_left;
    if (to_read > dst->capacity - 1) {
        to_read = dst->capacity - 1;
    }
    if (!pb_read(stream, (pb_byte_t *)dst->buf, to_read)) {
        return false;
    }
    dst->len = to_read;
    dst->buf[to_read] = '\0';
    return true;
}

/**
 * WebSocket message handler for Feishu
 * Receives raw protobuf Frame bytes, decodes payload JSON, then reuses existing logic.
 */
static void on_ws_message(gateway_t *gw, const char *session_id,
                         const char *content, size_t content_len, void *user_data)
{
    (void)gw;
    (void)session_id;
    (void)user_data;
    
    if (!content || content_len == 0) {
        return;
    }

    const uint8_t *data = (const uint8_t *)content;
    pb_istream_t stream = pb_istream_from_buffer(data, content_len);
    pbbp2_Frame frame = pbbp2_Frame_init_zero;
    feishu_frame_meta_t hdr = {0};
    hdr.sum = 0;
    hdr.seq = -1;
    hdr.kv_count = 0;

    char payload_buf[4096];
    feishu_payload_buf_t payload = {
        .buf = payload_buf,
        .capacity = sizeof(payload_buf),
        .len = 0
    };

    frame.payload.funcs.decode = feishu_payload_decode_cb;
    frame.payload.arg = &payload;
    frame.headers.funcs.decode = feishu_headers_decode_cb;
    frame.headers.arg = &hdr;

    if (!pb_decode(&stream, pbbp2_Frame_fields, &frame)) {
        MIMI_LOGW(TAG, "Failed to decode Feishu Frame: %s", PB_GET_ERROR(&stream));
        return;
    }

    if (payload.len == 0) {
        MIMI_LOGW(TAG, "Feishu Frame has empty payload");
        return;
    }

    /* Only handle + ACK event/card frames. Ignore control/unknown frames to avoid ACK loops. */
    if (hdr.type[0] && strcmp(hdr.type, "event") != 0 && strcmp(hdr.type, "card") != 0) {
        MIMI_LOGD(TAG, "Feishu WS frame type=%s ignored", hdr.type);
        return;
    }

    MIMI_LOGI(TAG, "Feishu WS payload JSON: %.200s", payload.buf);

    cJSON *root = cJSON_Parse(payload.buf);
    if (!root) {
        MIMI_LOGW(TAG, "Invalid Feishu payload JSON");
        return;
    }

    /* If we receive an ACK/response payload (e.g. {"code":200}), do not ACK it again. */
    {
        cJSON *schema = cJSON_GetObjectItem(root, "schema");
        cJSON *code = cJSON_GetObjectItem(root, "code");
        if (!schema && code && cJSON_IsNumber(code)) {
            MIMI_LOGD(TAG, "Feishu WS response payload received (code=%d), ignore", code->valueint);
            cJSON_Delete(root);
            return;
        }
    }

    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event = cJSON_GetObjectItem(root, "event");

    if (header && event) {
        cJSON *event_type = cJSON_GetObjectItem(header, "event_type");
        if (event_type && cJSON_IsString(event_type) && 
            strcmp(event_type->valuestring, "im.message.receive_v1") == 0) {
            
            cJSON *sender = cJSON_GetObjectItem(event, "sender");
            cJSON *message = cJSON_GetObjectItem(event, "message");
            
            if (sender && message) {
                /* Get sender_id from sender.sender_id.open_id */
                cJSON *sender_id_obj = cJSON_GetObjectItem(sender, "sender_id");
                cJSON *sender_id = NULL;
                if (sender_id_obj) {
                    sender_id = cJSON_GetObjectItem(sender_id_obj, "open_id");
                }
                
                /* Get chat_id from message.chat_id for p2p reply */
                cJSON *chat_id = cJSON_GetObjectItem(message, "chat_id");
                cJSON *chat_type = cJSON_GetObjectItem(message, "chat_type");
                cJSON *message_type = cJSON_GetObjectItem(message, "message_type");
                cJSON *message_id = cJSON_GetObjectItem(message, "message_id");
                cJSON *msg_content = cJSON_GetObjectItem(message, "content");
                
                MIMI_LOGI(TAG, "Parsed: sender_id=%s, chat_id=%s, chat_type=%s",
                          sender_id ? sender_id->valuestring : "(null)",
                          chat_id ? chat_id->valuestring : "(null)",
                          chat_type && cJSON_IsString(chat_type) ? chat_type->valuestring : "(null)");
                
                /* Use chat_id as user_id for reply (following Python SDK pattern) */
                const char *reply_id = chat_id ? chat_id->valuestring : NULL;
                
                if (reply_id && msg_content && cJSON_IsString(msg_content)) {
                    const char *mt = (message_type && cJSON_IsString(message_type)) ? message_type->valuestring : "unknown";
                    const char *route_text = NULL;
                    char route_buf[512] = {0};

                    cJSON *content_obj = cJSON_Parse(msg_content->valuestring);
                    if (!content_obj) {
                        snprintf(route_buf, sizeof(route_buf), "[feishu:%s] (content parse failed)", mt);
                        route_text = route_buf;
                    } else if (strcmp(mt, "text") == 0) {
                        cJSON *text = cJSON_GetObjectItem(content_obj, "text");
                        if (text && cJSON_IsString(text)) {
                            /* Trim whitespace from text content */
                            const char *raw_text = text->valuestring;
                            /* Skip leading whitespace */
                            while (*raw_text && isspace((unsigned char)*raw_text)) {
                                raw_text++;
                            }
                            /* Check if it's a command */
                            if (raw_text[0] == '/') {
                                route_text = raw_text;
                            } else {
                                route_text = text->valuestring;
                            }
                        } else {
                            snprintf(route_buf, sizeof(route_buf), "[feishu:text] (missing text field)");
                            route_text = route_buf;
                        }
                    } else if (strcmp(mt, "image") == 0) {
                        cJSON *image_key = cJSON_GetObjectItem(content_obj, "image_key");
                        if (image_key && cJSON_IsString(image_key)) {
                            char dl_url[512];
                            if (message_id && cJSON_IsString(message_id) && message_id->valuestring && message_id->valuestring[0]) {
                                /* Prefer message resource API for media from messages */
                                snprintf(dl_url, sizeof(dl_url),
                                         "https://open.feishu.cn/open-apis/im/v1/messages/%s/resources/%s?type=image",
                                         message_id->valuestring, image_key->valuestring);
                            } else {
                                snprintf(dl_url, sizeof(dl_url),
                                         "https://open.feishu.cn/open-apis/im/v1/images/%s",
                                         image_key->valuestring);
                            }
                            char out_base[512];
                            snprintf(out_base, sizeof(out_base), "downloads/feishu/images/%s", image_key->valuestring);
                            char saved_path[512];
                            mimi_err_t derr = feishu_download_to_file(dl_url, out_base, s_priv.tenant_access_token,
                                                                      saved_path, sizeof(saved_path));
                            if (derr == MIMI_OK) {
                                snprintf(route_buf, sizeof(route_buf),
                                         "[feishu:image] saved=%.192s image_key=%.192s",
                                         saved_path, image_key->valuestring);
                            } else {
                                snprintf(route_buf, sizeof(route_buf),
                                         "[feishu:image] download_failed=%d image_key=%.192s",
                                         derr, image_key->valuestring);
                            }
                        } else {
                            snprintf(route_buf, sizeof(route_buf), "[feishu:image] (missing image_key)");
                        }
                        route_text = route_buf;
                    } else if (strcmp(mt, "audio") == 0) {
                        cJSON *file_key = cJSON_GetObjectItem(content_obj, "file_key");
                        cJSON *duration = cJSON_GetObjectItem(content_obj, "duration");
                        if (file_key && cJSON_IsString(file_key)) {
                            char dl_url[512];
                            if (message_id && cJSON_IsString(message_id) && message_id->valuestring && message_id->valuestring[0]) {
                                snprintf(dl_url, sizeof(dl_url),
                                         "https://open.feishu.cn/open-apis/im/v1/messages/%s/resources/%s?type=file",
                                         message_id->valuestring, file_key->valuestring);
                            } else {
                                snprintf(dl_url, sizeof(dl_url),
                                         "https://open.feishu.cn/open-apis/im/v1/files/%s",
                                         file_key->valuestring);
                            }
                            char out_base[512];
                            snprintf(out_base, sizeof(out_base), "downloads/feishu/audio/%s", file_key->valuestring);
                            char saved_path[512];
                            mimi_err_t derr = feishu_download_to_file(dl_url, out_base, s_priv.tenant_access_token,
                                                                      saved_path, sizeof(saved_path));
                            if (derr == MIMI_OK) {
                                if (duration && cJSON_IsNumber(duration)) {
                                    snprintf(route_buf, sizeof(route_buf),
                                             "[feishu:audio] saved=%.192s duration=%d file_key=%.96s",
                                             saved_path, duration->valueint, file_key->valuestring);
                                } else {
                                    snprintf(route_buf, sizeof(route_buf),
                                             "[feishu:audio] saved=%.192s file_key=%.192s",
                                             saved_path, file_key->valuestring);
                                }
                            } else {
                                snprintf(route_buf, sizeof(route_buf),
                                         "[feishu:audio] download_failed=%d file_key=%.192s",
                                         derr, file_key->valuestring);
                            }
                        } else {
                            snprintf(route_buf, sizeof(route_buf), "[feishu:audio] (missing file_key)");
                        }
                        route_text = route_buf;
                    } else if (strcmp(mt, "file") == 0) {
                        cJSON *file_key = cJSON_GetObjectItem(content_obj, "file_key");
                        cJSON *file_name = cJSON_GetObjectItem(content_obj, "file_name");
                        if (file_key && cJSON_IsString(file_key)) {
                            char dl_url[512];
                            if (message_id && cJSON_IsString(message_id) && message_id->valuestring && message_id->valuestring[0]) {
                                snprintf(dl_url, sizeof(dl_url),
                                         "https://open.feishu.cn/open-apis/im/v1/messages/%s/resources/%s?type=file",
                                         message_id->valuestring, file_key->valuestring);
                            } else {
                                snprintf(dl_url, sizeof(dl_url),
                                         "https://open.feishu.cn/open-apis/im/v1/files/%s",
                                         file_key->valuestring);
                            }
                            char out_base[512];
                            snprintf(out_base, sizeof(out_base), "downloads/feishu/files/%s", file_key->valuestring);
                            char saved_path[512];
                            mimi_err_t derr = feishu_download_to_file(dl_url, out_base, s_priv.tenant_access_token,
                                                                      saved_path, sizeof(saved_path));
                            if (derr == MIMI_OK) {
                                if (file_name && cJSON_IsString(file_name)) {
                                    snprintf(route_buf, sizeof(route_buf),
                                             "[feishu:file] saved=%.192s file_name=%.96s file_key=%.96s",
                                             saved_path, file_name->valuestring, file_key->valuestring);
                                } else {
                                    snprintf(route_buf, sizeof(route_buf),
                                             "[feishu:file] saved=%.192s file_key=%.192s",
                                             saved_path, file_key->valuestring);
                                }
                            } else {
                                if (file_name && cJSON_IsString(file_name)) {
                                    snprintf(route_buf, sizeof(route_buf),
                                             "[feishu:file] download_failed=%d file_name=%.96s file_key=%.96s",
                                             derr, file_name->valuestring, file_key->valuestring);
                                } else {
                                    snprintf(route_buf, sizeof(route_buf),
                                             "[feishu:file] download_failed=%d file_key=%.192s",
                                             derr, file_key->valuestring);
                                }
                            }
                        } else {
                            snprintf(route_buf, sizeof(route_buf), "[feishu:file] (missing file_key)");
                        }
                        route_text = route_buf;
                    } else {
                        snprintf(route_buf, sizeof(route_buf), "[feishu:%s] %s", mt, msg_content->valuestring);
                        route_text = route_buf;
                    }

                    if (route_text) {
                        MIMI_LOGI(TAG, "Feishu routed content for chat_id=%s: %s",
                                  reply_id, route_text);
                        handle_message(reply_id, route_text);
                    }

                    cJSON_Delete(content_obj);
                } else {
                    MIMI_LOGW(TAG, "Missing reply_id or msg_content");
                }
            } else {
                MIMI_LOGW(TAG, "Missing sender or message in event");
            }
        }
    }

    /* Build minimal Response JSON and ACK frame back to Feishu for real events/cards. */
    {
        cJSON *resp = cJSON_CreateObject();
        if (resp) {
            cJSON_AddNumberToObject(resp, "code", 200);
            char *resp_json = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);

            if (resp_json) {
                feishu_bytes_t resp_bytes = {
                    .buf = (const uint8_t *)resp_json,
                    .len = strlen(resp_json),
                };

                pbbp2_Frame ack = pbbp2_Frame_init_zero;
                ack.SeqID = frame.SeqID;
                ack.LogID = frame.LogID;
                ack.service = frame.service;
                ack.method = frame.method;
                ack.headers.funcs.encode = feishu_headers_encode_cb;
                ack.headers.arg = &hdr;
                ack.payload.funcs.encode = feishu_payload_encode_cb;
                ack.payload.arg = &resp_bytes;

                uint8_t out_buf[1024];
                pb_ostream_t out = pb_ostream_from_buffer(out_buf, sizeof(out_buf));
                if (pb_encode(&out, pbbp2_Frame_fields, &ack)) {
                    ws_client_gateway_send_raw(out_buf, out.bytes_written);
                } else {
                    MIMI_LOGW(TAG, "Failed to encode Feishu ACK Frame: %s", PB_GET_ERROR(&out));
                }

                free(resp_json);
            }
        }
    }

    cJSON_Delete(root);
}

/**
 * Initialize Feishu Channel
 */
mimi_err_t feishu_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "Feishu Channel already initialized");
        return MIMI_OK;
    }

    /* Check if Feishu is enabled */
    const mimi_config_t *config = mimi_config_get();
    MIMI_LOGI(TAG, "Feishu config: enabled=%d, app_id=%s", config->feishu_enabled, config->feishu_app_id);
    if (!config->feishu_enabled) {
        MIMI_LOGI(TAG, "Feishu Channel is disabled");
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Load credentials from config */
    if (config->feishu_app_id[0] != '\0') {
        strncpy(s_priv.app_id, config->feishu_app_id, sizeof(s_priv.app_id) - 1);
    }
    if (config->feishu_app_secret[0] != '\0') {
        strncpy(s_priv.app_secret, config->feishu_app_secret, sizeof(s_priv.app_secret) - 1);
    }

    if (!s_priv.app_id[0] || !s_priv.app_secret[0]) {
        MIMI_LOGW(TAG, "Feishu credentials not configured");
    } else {
        MIMI_LOGI(TAG, "Feishu initialized with App ID: %.6s***", s_priv.app_id);
    }

    /* Get or create WebSocket Client Gateway */
    s_priv.ws_gateway = gateway_manager_find("ws_client");
    if (!s_priv.ws_gateway) {
        MIMI_LOGE(TAG, "WebSocket Client Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Set WebSocket message handler early to avoid missing initial messages */
    gateway_set_on_message(s_priv.ws_gateway, on_ws_message, ch);

    /* Get or create HTTP Gateway */
    s_priv.http_gateway = gateway_manager_find("http");
    if (!s_priv.http_gateway) {
        MIMI_LOGE(TAG, "HTTP Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Configure HTTP Gateway for Feishu (base domain only) */
    mimi_err_t err = http_client_gateway_configure("https://open.feishu.cn",
                                            NULL, 30000);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to configure HTTP Gateway: %d", err);
        return err;
    }

    /* Register mapping for Input Processor */
    err = router_register_mapping("feishu", "feishu");
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to register input processor mapping");
        return err;
    }

    ch->priv_data = &s_priv;
    s_priv.initialized = true;

    MIMI_LOGI(TAG, "Feishu Channel initialized");
    return MIMI_OK;
}

/**
 * Start Feishu Channel
 */
mimi_err_t feishu_channel_start_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        MIMI_LOGE(TAG, "Feishu Channel not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        MIMI_LOGW(TAG, "Feishu Channel already started");
        return MIMI_OK;
    }

    if (!s_priv.ws_gateway || !s_priv.http_gateway) {
        MIMI_LOGW(TAG, "Cannot start Feishu without WebSocket and HTTP Gateways");
        return MIMI_ERR_INVALID_STATE;
    }

    /* Start HTTP Gateway */
    mimi_err_t err = gateway_start(s_priv.http_gateway);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start HTTP Gateway: %d", err);
        return err;
    }

    /* Kick off async startup state machine (non-blocking) */
    s_priv.stopping = false;
    s_priv.running = false;
    s_priv.started = false;
    feishu_sm_start(ch);

    /* Return immediately; subsequent steps run on dispatcher worker callbacks. */
    return MIMI_OK;
}

/**
 * Stop Feishu Channel
 */
mimi_err_t feishu_channel_stop_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return MIMI_OK;
    }

    s_priv.stopping = true;
    s_priv.running = false;
    s_priv.started = false;
    s_priv.sm_state = FEISHU_SM_IDLE;

    /* Stop WebSocket Gateway */
    if (s_priv.ws_gateway) {
        gateway_stop(s_priv.ws_gateway);
    }

    /* Stop HTTP Gateway */
    if (s_priv.http_gateway) {
        gateway_stop(s_priv.http_gateway);
    }

    MIMI_LOGI(TAG, "Feishu Channel stopped");
    return MIMI_OK;
}

/**
 * Destroy Feishu Channel
 */
void feishu_channel_destroy_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return;
    }

    feishu_channel_stop_impl(ch);

    /* Unregister mapping */
    router_unregister_mapping("feishu");

    memset(&s_priv, 0, sizeof(s_priv));

    MIMI_LOGI(TAG, "Feishu Channel destroyed");
}

#define FEISHU_IMAGE_PREFIX "[feishu:image]path="
#define FEISHU_AUDIO_PREFIX "[feishu:audio]path="

/**
 * Send message through Feishu Channel.
 * Content can be:
 * - Plain text -> send as text message.
 * - "[feishu:image]path=<path>" -> upload image from file, send as image message.
 * - "[feishu:audio]path=<path>" -> upload file as voice, send as audio message.
 */
mimi_err_t feishu_channel_send_impl(channel_t *ch, const char *session_id,
                                    const char *content)
{
    (void)ch;

    if (!s_priv.initialized || !s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }
    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (strncmp(content, FEISHU_IMAGE_PREFIX, sizeof(FEISHU_IMAGE_PREFIX) - 1) == 0) {
        const char *path = content + (sizeof(FEISHU_IMAGE_PREFIX) - 1);
        char image_key[128];
        mimi_err_t err = feishu_upload_image_from_file(
            path, "message", s_priv.tenant_access_token,
            image_key, sizeof(image_key));
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Feishu image upload failed: %d, falling back to text", err);
            return feishu_send_message(session_id, content);
        }
        return feishu_send_image(session_id, image_key);
    }

    if (strncmp(content, FEISHU_AUDIO_PREFIX, sizeof(FEISHU_AUDIO_PREFIX) - 1) == 0) {
        const char *path = content + (sizeof(FEISHU_AUDIO_PREFIX) - 1);
        char file_key[128];
        mimi_err_t err = feishu_upload_file_from_file(
            path, "opus", NULL, 0, s_priv.tenant_access_token,
            file_key, sizeof(file_key));
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Feishu audio upload failed: %d, falling back to text", err);
            return feishu_send_message(session_id, content);
        }
        return feishu_send_audio(session_id, file_key);
    }

    return feishu_send_message(session_id, content);
}

/**
 * Check if Feishu Channel is running
 */
static bool feishu_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.running;
}

/**
 * Set message callback (not used - WebSocket is used)
 */
static void feishu_set_on_message(channel_t *ch,
                                void (*cb)(channel_t *, const char *, 
                                           const char *, void *),
                                void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
    /* Messages are handled via WebSocket */
}

/**
 * Set connect callback (not used)
 */
static void feishu_set_on_connect(channel_t *ch,
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
static void feishu_set_on_disconnect(channel_t *ch,
                                   void (*cb)(channel_t *, const char *, 
                                              void *),
                                   void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Global Feishu Channel instance
 */
channel_t g_feishu_channel = {
    .name = "feishu",
    .description = "Feishu (Lark) Bot",
    .require_auth = false,
    .max_sessions = 0,
    .init = feishu_channel_init_impl,
    .start = feishu_channel_start_impl,
    .stop = feishu_channel_stop_impl,
    .destroy = feishu_channel_destroy_impl,
    .send = feishu_channel_send_impl,
    .is_running = feishu_is_running_impl,
    .set_on_message = feishu_set_on_message,
    .set_on_connect = feishu_set_on_connect,
    .set_on_disconnect = feishu_set_on_disconnect,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false
};

/**
 * Initialize Feishu Channel module
 */
mimi_err_t feishu_channel_init(void)
{
    return MIMI_OK;
}
