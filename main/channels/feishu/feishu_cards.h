#pragma once

#include <stddef.h>
#include "mimi_err.h"
#include "bus/message_bus.h"
#include "channels/feishu/feishu_card_model.h"

/* Public streaming API: start and update a markdown-only card. */
mimi_err_t feishu_stream_start(const char *chat_id,
                               const char *content_md,
                               char *out_msg_id,
                               size_t out_len);

mimi_err_t feishu_stream_update(const char *message_id,
                                const char *content_md);

/* Update any existing interactive message using a structured card model. */
mimi_err_t feishu_update_interactive(const char *message_id, const feishu_card_model_t *model);

/* Basic message sending helpers (chat_id for p2p). */
mimi_err_t feishu_send_text(const char *chat_id, const char *text);
mimi_err_t feishu_send_image_key(const char *chat_id, const char *image_key);
mimi_err_t feishu_send_audio_key(const char *chat_id, const char *file_key);

/* Control card helpers (used by channel send_control and ws action callbacks). */
mimi_err_t feishu_send_control_card(const char *chat_id,
                                    mimi_control_type_t control_type,
                                    const char *request_id,
                                    const char *target,
                                    const char *data);

mimi_err_t feishu_update_control_card_result(const char *message_id, const char *text);

