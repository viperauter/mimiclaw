/**
 * Feishu WebSocket Frame decoding, ACK, and event handling.
 */

#include "channels/feishu/feishu_ws.h"
#include "channels/feishu/feishu_priv.h"
#include "channels/feishu/feishu_cards.h"

#include "router/router.h"
#include "bus/message_bus.h"
#include "gateway/websocket/ws_client_gateway.h"
#include "log.h"

#include "cJSON.h"
#include "channels/feishu/pb/pbbp2.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "feishu";

static void handle_message(const char *user_id, const char *content)
{
    MIMI_LOGI(TAG, "Incoming message from %s: %.40s...", user_id, content);
    router_handle_generic("feishu", user_id, content);
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

/* Forward declarations (used by early-ACK helper). */
static bool feishu_payload_encode_cb(pb_ostream_t *stream, const pb_field_t *field, void *const *arg);
static bool feishu_headers_encode_cb(pb_ostream_t *stream, const pb_field_t *field, void *const *arg);

static void feishu_ws_send_ack_if_needed(const pbbp2_Frame *frame, const feishu_frame_meta_t *hdr)
{
    if (!frame || !hdr) return;

    cJSON *resp = cJSON_CreateObject();
    if (!resp) return;
    cJSON_AddNumberToObject(resp, "code", 200);
    char *resp_json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!resp_json) return;

    feishu_bytes_t resp_bytes = {
        .buf = (const uint8_t *)resp_json,
        .len = strlen(resp_json),
    };

    pbbp2_Frame ack = pbbp2_Frame_init_zero;
    ack.SeqID = frame->SeqID;
    ack.LogID = frame->LogID;
    ack.service = frame->service;
    ack.method = frame->method;
    ack.headers.funcs.encode = feishu_headers_encode_cb;
    ack.headers.arg = (void *)hdr;
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

void feishu_on_ws_message(gateway_t *gw, const char *session_id,
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

    /* Optional card update info: we fill these when handling card.action.trigger
     * and only PATCH the card AFTER sending WS ACK to avoid flicker. */
    char update_msg_id[128] = {0};
    char update_text[256] = {0};
    bool have_update = false;
    bool ack_sent = false;

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

    /* ACK as early as possible to avoid Feishu retransmitting events when our
     * downstream routing is slow. */
    feishu_ws_send_ack_if_needed(&frame, &hdr);
    ack_sent = true;

    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event = cJSON_GetObjectItem(root, "event");

    if (header && event) {
        cJSON *event_type = cJSON_GetObjectItem(header, "event_type");
        if (event_type && cJSON_IsString(event_type) &&
            strcmp(event_type->valuestring, "im.message.receive_v1") == 0) {

            cJSON *message = cJSON_GetObjectItem(event, "message");
            if (message) {
                cJSON *chat_id = cJSON_GetObjectItem(message, "chat_id");
                cJSON *message_type = cJSON_GetObjectItem(message, "message_type");
                cJSON *msg_content = cJSON_GetObjectItem(message, "content");

                const char *reply_id = (chat_id && cJSON_IsString(chat_id)) ? chat_id->valuestring : NULL;

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
                            const char *raw_text = text->valuestring;
                            while (*raw_text && isspace((unsigned char)*raw_text)) raw_text++;
                            route_text = (raw_text[0] == '/') ? raw_text : text->valuestring;
                        } else {
                            snprintf(route_buf, sizeof(route_buf), "[feishu:text] (missing text field)");
                            route_text = route_buf;
                        }
                    } else {
                        /* Keep existing behavior: route non-text as a tagged string. */
                        snprintf(route_buf, sizeof(route_buf), "[feishu:%s] %s", mt, msg_content->valuestring);
                        route_text = route_buf;
                    }

                    if (route_text) {
                        MIMI_LOGI(TAG, "Feishu routed content for chat_id=%s: %s", reply_id, route_text);
                        handle_message(reply_id, route_text);
                    }

                    cJSON_Delete(content_obj);
                } else {
                    MIMI_LOGW(TAG, "Missing reply_id or msg_content");
                }
            }
        } else if (event_type && cJSON_IsString(event_type) &&
                   strcmp(event_type->valuestring, "card.action.trigger") == 0) {
            cJSON *action = cJSON_GetObjectItem(event, "action");
            cJSON *value = action ? cJSON_GetObjectItem(action, "value") : NULL;
            cJSON *ctx = cJSON_GetObjectItem(event, "context");
            cJSON *open_msg_id = ctx ? cJSON_GetObjectItem(ctx, "open_message_id") : NULL;

            const char *open_msg_id_str = (open_msg_id && cJSON_IsString(open_msg_id)) ? open_msg_id->valuestring : "";

            const char *resp_str = NULL;
            const char *req_id_str = NULL;

            if (value && cJSON_IsObject(value)) {
                cJSON *act = cJSON_GetObjectItem(value, "action");
                cJSON *req = cJSON_GetObjectItem(value, "request_id");
                if (act && cJSON_IsString(act) && req && cJSON_IsString(req)) {
                    req_id_str = req->valuestring;
                    if (strcmp(act->valuestring, "ACCEPT") == 0) resp_str = "ACCEPT";
                    else if (strcmp(act->valuestring, "ACCEPT_ALWAYS") == 0) resp_str = "ACCEPT_ALWAYS";
                    else if (strcmp(act->valuestring, "REJECT") == 0) resp_str = "REJECT";
                }
            }

            if (resp_str && req_id_str && req_id_str[0]) {
                MIMI_LOGI(TAG, "Feishu card action: request_id=%s, response=%s", req_id_str, resp_str);

                mimi_msg_t msg = {0};
                strncpy(msg.channel, "feishu", sizeof(msg.channel) - 1);
                msg.type = MIMI_MSG_TYPE_CONTROL;
                msg.control_type = MIMI_CONTROL_TYPE_CONFIRM;
                strncpy(msg.request_id, req_id_str, sizeof(msg.request_id) - 1);
                msg.content = strdup(resp_str);
                if (!msg.content) {
                    cJSON_Delete(root);
                    return;
                }
                mimi_err_t berr = message_bus_push_inbound(&msg);
                if (berr != MIMI_OK) {
                    MIMI_LOGE(TAG, "Failed to push Feishu control response to bus: %s", mimi_err_to_name(berr));
                    free(msg.content);
                }

                /* Best-effort update of the interactive card to show result. */
                if (open_msg_id_str && open_msg_id_str[0]) {
                    /* Unfreeze the stream card (if this action belongs to it). */
                    feishu_stream_unlock_for_message_id(open_msg_id_str);
                    if (strcmp(resp_str, "ACCEPT") == 0) {
                        snprintf(update_text, sizeof(update_text), "✅ **已授权：允许本次执行**\n\n接下来将开始执行。");
                    } else if (strcmp(resp_str, "ACCEPT_ALWAYS") == 0) {
                        snprintf(update_text, sizeof(update_text), "🔓 **已永久授权**\n\n接下来将开始执行。");
                    } else if (strcmp(resp_str, "REJECT") == 0) {
                        snprintf(update_text, sizeof(update_text), "❌ **已拒绝授权**\n\n本次操作不会执行。");
                    }
                    strncpy(update_msg_id, open_msg_id_str, sizeof(update_msg_id) - 1);
                    have_update = true;
                }
            }
        }
    }

    /* Only update the card AFTER ACK has been sent to avoid flicker. */
    if (ack_sent && have_update && update_msg_id[0] && update_text[0]) {
        /* If this message_id is our active stream card, keep stream styling;
         * otherwise update as a standalone control/result card. */
        if (feishu_stream_message_id_is_active(update_msg_id)) {
            /* Append to timeline and flush. */
            const char *base = feishu_stream_timeline_get_for_message_id(update_msg_id);
            if (base && base[0]) {
                char merged[8192];
                snprintf(merged, sizeof(merged), "%s\n\n---\n\n%s", base, update_text);
                (void)feishu_stream_update(update_msg_id, merged);
            } else {
                (void)feishu_stream_update(update_msg_id, update_text);
            }
        } else {
            (void)feishu_update_control_card_result(update_msg_id, update_text);
        }
    }

    cJSON_Delete(root);
}

