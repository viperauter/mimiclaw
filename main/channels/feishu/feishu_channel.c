/**
 * Feishu (Lark) Channel Implementation
 *
 * Uses WebSocket Client Gateway for Feishu Bot integration.
 * Provides real-time message handling via WebSocket connection.
 */

#include "channels/feishu/feishu_channel.h"
#include "channels/feishu/feishu_media.h"
#include "channels/feishu/feishu_priv.h"
#include "channels/feishu/feishu_cards.h"
#include "channels/feishu/feishu_startup.h"
#include "channels/feishu/feishu_ws.h"
#include "channels/channel_manager.h"
#include "router/router.h"
#include "commands/command.h"
#include "config.h"
#include "config_view.h"
#include "bus/message_bus.h"
#include "gateway/websocket/ws_client_gateway.h"
#include "gateway/http/http_client_gateway.h"
#include "gateway/gateway_manager.h"
#include "log.h"
#include "os/os.h"
#include "fs/fs.h"

#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

static const char *TAG = "feishu";

static feishu_channel_priv_t s_priv = {0};

feishu_channel_priv_t *feishu_priv_get(void)
{
    return &s_priv;
}

/* Forward declarations */
static bool feishu_is_running_impl(channel_t *ch);
#include <stdbool.h>

/* card helpers moved to feishu_cards.c */

/* Simple per-chat streaming state for agent turn status. */
typedef struct {
    char chat_id[128];
    char message_id[128];
    bool active;
    bool locked;
    char timeline_md[8192];
    size_t timeline_len;
} feishu_stream_state_t;

/* Allow limited concurrent chats with streaming cards. */
#define FEISHU_MAX_STREAM_STATES 16
static feishu_stream_state_t s_stream_states[FEISHU_MAX_STREAM_STATES] = {0};

bool feishu_stream_message_id_is_active(const char *message_id)
{
    if (!message_id || !message_id[0]) return false;
    for (size_t i = 0; i < FEISHU_MAX_STREAM_STATES; i++) {
        if (s_stream_states[i].active &&
            s_stream_states[i].message_id[0] &&
            strcmp(s_stream_states[i].message_id, message_id) == 0) {
            return true;
        }
    }
    return false;
}

void feishu_stream_lock_for_message_id(const char *message_id)
{
    if (!message_id || !message_id[0]) return;
    for (size_t i = 0; i < FEISHU_MAX_STREAM_STATES; i++) {
        if (s_stream_states[i].active &&
            s_stream_states[i].message_id[0] &&
            strcmp(s_stream_states[i].message_id, message_id) == 0) {
            s_stream_states[i].locked = true;
            return;
        }
    }
}

void feishu_stream_unlock_for_message_id(const char *message_id)
{
    if (!message_id || !message_id[0]) return;
    for (size_t i = 0; i < FEISHU_MAX_STREAM_STATES; i++) {
        if (s_stream_states[i].active &&
            s_stream_states[i].message_id[0] &&
            strcmp(s_stream_states[i].message_id, message_id) == 0) {
            s_stream_states[i].locked = false;
            return;
        }
    }
}

bool feishu_stream_is_locked_for_chat(const char *chat_id)
{
    if (!chat_id || !chat_id[0]) return false;
    for (size_t i = 0; i < FEISHU_MAX_STREAM_STATES; i++) {
        if (s_stream_states[i].active &&
            s_stream_states[i].chat_id[0] &&
            strcmp(s_stream_states[i].chat_id, chat_id) == 0) {
            return s_stream_states[i].locked;
        }
    }
    return false;
}

static feishu_stream_state_t *feishu_stream_state_find(const char *chat_id, bool create)
{
    if (!chat_id || !chat_id[0]) {
        return NULL;
    }

    /* First, look for existing state. */
    for (size_t i = 0; i < FEISHU_MAX_STREAM_STATES; i++) {
        if (s_stream_states[i].active &&
            strncmp(s_stream_states[i].chat_id, chat_id, sizeof(s_stream_states[i].chat_id) - 1) == 0) {
            return &s_stream_states[i];
        }
    }

    if (!create) {
        return NULL;
    }

    /* Find a free slot. */
    for (size_t i = 0; i < FEISHU_MAX_STREAM_STATES; i++) {
        if (!s_stream_states[i].active) {
            memset(&s_stream_states[i], 0, sizeof(s_stream_states[i]));
            strncpy(s_stream_states[i].chat_id, chat_id, sizeof(s_stream_states[i].chat_id) - 1);
            s_stream_states[i].active = true;
            return &s_stream_states[i];
        }
    }

    return NULL;
}

void feishu_stream_timeline_reset_for_chat(const char *chat_id, const char *initial_md)
{
    feishu_stream_state_t *st = feishu_stream_state_find(chat_id, false);
    if (!st) return;
    st->timeline_len = 0;
    st->timeline_md[0] = '\0';
    if (initial_md && initial_md[0]) {
        strncpy(st->timeline_md, initial_md, sizeof(st->timeline_md) - 1);
        st->timeline_md[sizeof(st->timeline_md) - 1] = '\0';
        st->timeline_len = strlen(st->timeline_md);
    }
}

static void timeline_append(feishu_stream_state_t *st, const char *md)
{
    if (!st || !md || !md[0]) return;
    size_t cap = sizeof(st->timeline_md);
    size_t cur = st->timeline_len;
    if (cur >= cap - 1) return;

    /* Separator between events */
    const char *sep = "\n\n---\n\n";
    size_t sep_len = strlen(sep);
    if (cur > 0 && cur + sep_len < cap - 1) {
        memcpy(st->timeline_md + cur, sep, sep_len);
        cur += sep_len;
        st->timeline_md[cur] = '\0';
    }

    size_t add = strlen(md);
    if (add > cap - 1 - cur) add = cap - 1 - cur;
    memcpy(st->timeline_md + cur, md, add);
    cur += add;
    st->timeline_md[cur] = '\0';
    st->timeline_len = cur;
}

void feishu_stream_timeline_append_for_chat(const char *chat_id, const char *md, bool patch_if_unlocked)
{
    feishu_stream_state_t *st = feishu_stream_state_find(chat_id, false);
    if (!st || !st->active || !st->message_id[0]) return;

    timeline_append(st, md);

    if (patch_if_unlocked && !st->locked) {
        (void)feishu_stream_update(st->message_id, st->timeline_md);
    }
}

const char *feishu_stream_timeline_get_for_message_id(const char *message_id)
{
    if (!message_id || !message_id[0]) return NULL;
    for (size_t i = 0; i < FEISHU_MAX_STREAM_STATES; i++) {
        if (s_stream_states[i].active &&
            s_stream_states[i].message_id[0] &&
            strcmp(s_stream_states[i].message_id, message_id) == 0) {
            return s_stream_states[i].timeline_md;
        }
    }
    return NULL;
}

/**
 * WebSocket message handler for Feishu
 * Receives raw protobuf Frame bytes, decodes payload JSON, then reuses existing logic.
 */
static void on_ws_message(gateway_t *gw, const char *session_id,
                          const char *content, size_t content_len, void *user_data)
{
    feishu_on_ws_message(gw, session_id, content, content_len, user_data);
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
    mimi_cfg_obj_t feishu = mimi_cfg_named("channels", "feishu");
    bool enabled = mimi_cfg_get_bool(feishu, "enabled", false);
    const char *app_id = mimi_cfg_get_str(feishu, "appId", "");
    MIMI_LOGD(TAG, "Feishu config: enabled=%d, app_id=%s", enabled, app_id ? app_id : "");
    if (!enabled) {
        MIMI_LOGD(TAG, "Feishu Channel is disabled");
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Load credentials from config */
    const char *secret = mimi_cfg_get_str(feishu, "appSecret", "");
    if (app_id && app_id[0] != '\0') strncpy(s_priv.app_id, app_id, sizeof(s_priv.app_id) - 1);
    if (secret && secret[0] != '\0') strncpy(s_priv.app_secret, secret, sizeof(s_priv.app_secret) - 1);

    if (!s_priv.app_id[0] || !s_priv.app_secret[0]) {
        MIMI_LOGW(TAG, "Feishu credentials not configured");
    } else {
        MIMI_LOGD(TAG, "Feishu initialized with App ID: %.6s***", s_priv.app_id);
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

    MIMI_LOGD(TAG, "Feishu Channel initialized");
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
            return feishu_send_text(session_id, content);
        }
        return feishu_send_image_key(session_id, image_key);
    }

    if (strncmp(content, FEISHU_AUDIO_PREFIX, sizeof(FEISHU_AUDIO_PREFIX) - 1) == 0) {
        const char *path = content + (sizeof(FEISHU_AUDIO_PREFIX) - 1);
        char file_key[128];
        mimi_err_t err = feishu_upload_file_from_file(
            path, "opus", NULL, 0, s_priv.tenant_access_token,
            file_key, sizeof(file_key));
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Feishu audio upload failed: %d, falling back to text", err);
            return feishu_send_text(session_id, content);
        }
        return feishu_send_audio_key(session_id, file_key);
    }

    return feishu_send_text(session_id, content);
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
 * Channel send_control implementation for Feishu: render confirmation as interactive card.
 */
static mimi_err_t feishu_channel_send_control(channel_t *ch, const char *session_id,
                                              mimi_control_type_t control_type,
                                              const char *request_id,
                                              const char *target,
                                              const char *data)
{
    (void)ch;
    if (control_type != MIMI_CONTROL_TYPE_CONFIRM) {
        /* For now, only confirmation requests are rendered as cards. */
        return feishu_send_text(session_id,
                                   "收到控制请求，但当前只支持确认类型 (CONFIRM)。");
    }

    /* Prefer fusing the confirmation UI into the active stream card if present. */
    feishu_stream_state_t *st = feishu_stream_state_find(session_id, false);
    if (st && st->active && st->message_id[0]) {
        /* Keep timeline moving even while locked; but do not patch immediately. */
        feishu_stream_timeline_append_for_chat(session_id, "🔐 **等待授权**", false);
        char desc_buf[512];
        snprintf(desc_buf, sizeof(desc_buf),
                 "🔐 **需要确认工具调用**\n\n"
                 "- 工具：`%s`\n"
                 "- 请求ID：`%.40s`\n\n"
                 "请在下方选择允许/拒绝。",
                 (target && target[0]) ? target : "(unknown)",
                 request_id ? request_id : "");

        feishu_card_button_t buttons[] = {
            { .text = "允许本次", .type = "primary", .value_action = "ACCEPT",        .value_request_id = request_id, .value_target = target },
            { .text = "总是允许", .type = "primary", .value_action = "ACCEPT_ALWAYS", .value_request_id = request_id, .value_target = target },
            { .text = "拒绝",     .type = NULL,      .value_action = "REJECT",        .value_request_id = request_id, .value_target = target },
        };
        const char *base_md = st->timeline_md[0] ? st->timeline_md : "";
        char merged_md[8192];
        size_t base_len = strlen(base_md);
        size_t desc_len = strlen(desc_buf);
        /* Keep within buffer: prefer preserving existing timeline. */
        if (base_len + desc_len + 8 >= sizeof(merged_md)) {
            snprintf(merged_md, sizeof(merged_md), "%s", base_md);
        } else {
            /* Two-step copy avoids -Wformat-truncation false positives. */
            memcpy(merged_md, base_md, base_len);
            merged_md[base_len] = '\0';
            strncat(merged_md, "\n\n---\n\n", sizeof(merged_md) - strlen(merged_md) - 1);
            strncat(merged_md, desc_buf, sizeof(merged_md) - strlen(merged_md) - 1);
        }

        feishu_card_block_t blocks[] = {
            { .type = FEISHU_CARD_BLOCK_MARKDOWN, .markdown = { .md = merged_md } },
            { .type = FEISHU_CARD_BLOCK_ACTIONS,  .actions  = { .buttons = buttons, .button_count = sizeof(buttons) / sizeof(buttons[0]) } },
        };
        feishu_card_model_t model = {
            .wide_screen_mode = true,
            .update_multi = true,
            .title = "Mimi",
            .subtitle = "等待授权",
            .blocks = blocks,
            .block_count = sizeof(blocks) / sizeof(blocks[0]),
        };

        mimi_err_t uerr = feishu_update_interactive(st->message_id, &model);
        if (uerr == MIMI_OK) {
            /* Prevent subsequent status updates from overwriting the buttons. */
            feishu_stream_lock_for_message_id(st->message_id);
            return MIMI_OK;
        }
        MIMI_LOGW(TAG, "Failed to fuse control into stream card (message_id=%s): %s",
                  st->message_id, mimi_err_to_name(uerr));
        /* Fall back to standalone control card below. */
    }

    return feishu_send_control_card(session_id, control_type, request_id, target, data);
}

static mimi_err_t feishu_channel_send_msg_impl(channel_t *ch, const mimi_msg_t *msg)
{
    (void)ch;
    if (!msg || !msg->chat_id[0]) {
        return MIMI_ERR_INVALID_ARG;
    }

    /* Map agent_turn STATUS messages to interactive stream cards so that
     * the async agent loop can provide a streaming-style experience on
     * Feishu without changing its core logic. */
    if (msg->type == MIMI_MSG_TYPE_STATUS &&
        msg->status_key[0] != '\0' &&
        strcmp(msg->status_key, "agent_turn") == 0) {
        if (msg->status_phase == MIMI_STATUS_PHASE_START) {
            feishu_stream_state_t *st = feishu_stream_state_find(msg->chat_id, true);
            if (!st) {
                MIMI_LOGW(TAG, "No stream state slot available for chat_id=%s", msg->chat_id);
                /* Fallback to plain text message. */
                return feishu_channel_send_impl(ch, msg->chat_id, msg->content ? msg->content : "");
            }

            char msg_id[128] = {0};
            const char *content_md = msg->content ? msg->content : "mimi is working...";
            mimi_err_t err = feishu_stream_start(msg->chat_id, content_md, msg_id, sizeof(msg_id));
            if (err == MIMI_OK && msg_id[0] != '\0') {
                strncpy(st->message_id, msg_id, sizeof(st->message_id) - 1);
                st->active = true;
                st->locked = false;
                feishu_stream_timeline_reset_for_chat(msg->chat_id, content_md);
                MIMI_LOGD(TAG, "Feishu stream started for chat_id=%s, message_id=%s",
                          msg->chat_id, st->message_id);
                return MIMI_OK;
            }

            MIMI_LOGW(TAG, "Feishu stream_start failed for chat_id=%s, err=%s",
                      msg->chat_id, mimi_err_to_name(err));
            /* Fallback to plain text if card creation fails. */
            return feishu_channel_send_impl(ch, msg->chat_id, msg->content ? msg->content : "");
        } else if (msg->status_phase == MIMI_STATUS_PHASE_PROGRESS ||
                   msg->status_phase == MIMI_STATUS_PHASE_DONE) {
            feishu_stream_state_t *st = feishu_stream_state_find(msg->chat_id, false);
            if (!st || !st->active || !st->message_id[0]) {
                /* No active stream: ignore STATUS updates to avoid sending duplicate
                 * plain-text messages when the final TEXT has already been delivered
                 * via stream card update. */
                return MIMI_OK;
            }

            const char *content_md = msg->content ? msg->content : "";
            /* Always accumulate; patch only when not locked. */
            timeline_append(st, content_md);
            if (!st->locked || msg->status_phase == MIMI_STATUS_PHASE_DONE) {
                mimi_err_t err = feishu_stream_update(st->message_id, st->timeline_md);
                if (err != MIMI_OK) {
                    MIMI_LOGW(TAG, "Feishu stream_update failed for chat_id=%s, message_id=%s, err=%s",
                              msg->chat_id, st->message_id, mimi_err_to_name(err));
                    /* Also try to send as plain text so user still sees something. */
                    return feishu_channel_send_impl(ch, msg->chat_id, content_md);
                }
            }

            if (msg->status_phase == MIMI_STATUS_PHASE_DONE) {
                memset(st, 0, sizeof(*st));
            }
            return MIMI_OK;
        }
    }

    /* If there is an active stream card for this chat and we now receive a
     * final TEXT response from the agent, update the existing card instead
     * of sending a separate message. This gives a natural streaming effect:
     * START status creates the card, and the final answer replaces it. */
    if (msg->type == MIMI_MSG_TYPE_TEXT) {
        feishu_stream_state_t *st = feishu_stream_state_find(msg->chat_id, false);
        if (st && st->active && st->message_id[0]) {
            const char *content_md = msg->content ? msg->content : "";
            timeline_append(st, content_md);
            mimi_err_t err = feishu_stream_update(st->message_id, st->timeline_md);
            if (err == MIMI_OK) {
                MIMI_LOGD(TAG, "Feishu stream final update for chat_id=%s, message_id=%s",
                          msg->chat_id, st->message_id);
                memset(st, 0, sizeof(*st));
                return MIMI_OK;
            }

            MIMI_LOGW(TAG, "Feishu stream final update failed for chat_id=%s, message_id=%s, err=%s",
                      msg->chat_id, st->message_id, mimi_err_to_name(err));
            /* Fall through to plain text on failure. */
        }
    }

    /* For all other messages, treat as plain text. */
    return feishu_channel_send_impl(ch, msg->chat_id, msg->content ? msg->content : "");
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
    .send_msg = feishu_channel_send_msg_impl,
    .is_running = feishu_is_running_impl,
    .set_on_message = feishu_set_on_message,
    .set_on_connect = feishu_set_on_connect,
    .set_on_disconnect = feishu_set_on_disconnect,
    .send_control = feishu_channel_send_control,
    .set_on_control_response = NULL,
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
