#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "mimi_err.h"
#include "gateway/gateway.h"

/* Feishu Channel private data (shared across internal modules). */
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

/* Access the singleton Feishu private state. */
feishu_channel_priv_t *feishu_priv_get(void);

/* Stream state query (used to decide whether a message_id is the active stream card). */
bool feishu_stream_message_id_is_active(const char *message_id);

/* Lock/unlock an active stream card to prevent status updates overwriting
 * a temporary UI state (e.g. permission buttons). */
void feishu_stream_lock_for_message_id(const char *message_id);
void feishu_stream_unlock_for_message_id(const char *message_id);
bool feishu_stream_is_locked_for_chat(const char *chat_id);

/* Append markdown to the active stream timeline and optionally patch immediately.
 * These helpers live in feishu_channel.c because it owns s_stream_states. */
void feishu_stream_timeline_reset_for_chat(const char *chat_id, const char *initial_md);
void feishu_stream_timeline_append_for_chat(const char *chat_id, const char *md, bool patch_if_unlocked);
const char *feishu_stream_timeline_get_for_message_id(const char *message_id);

