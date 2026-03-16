/**
 * Feishu (Lark) Channel Interface
 *
 * Provides Feishu Bot integration via HTTP Webhook
 */

#ifndef FEISHU_CHANNEL_H
#define FEISHU_CHANNEL_H

#include <stddef.h>
#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Feishu Channel instance
 */
extern channel_t g_feishu_channel;

/**
 * Initialize Feishu Channel module
 * Called before registering with Channel Manager
 * @return MIMI_OK on success
 */
mimi_err_t feishu_channel_init(void);

/**
 * Set Feishu app credentials
 * @param app_id Feishu App ID
 * @param app_secret Feishu App Secret
 * @return MIMI_OK on success
 */
mimi_err_t feishu_channel_set_credentials(const char *app_id, const char *app_secret);

/**
 * Start a streaming card in Feishu (one message that will be updated in-place).
 * This sends an interactive card with the given markdown content and returns
 * the Feishu message_id for subsequent updates.
 *
 * @param chat_id      Feishu chat_id to send to
 * @param content_md   Initial markdown content to display
 * @param out_msg_id   Buffer to receive Feishu message_id (null-terminated)
 * @param out_len      Size of out_msg_id buffer
 * @return MIMI_OK on success
 */
mimi_err_t feishu_stream_start(const char *chat_id,
                               const char *content_md,
                               char *out_msg_id,
                               size_t out_len);

/**
 * Update an existing streaming card by message_id with new markdown content.
 *
 * @param message_id   Feishu message_id previously returned by feishu_stream_start
 * @param content_md   New markdown content to render in the card
 * @return MIMI_OK on success
 */
mimi_err_t feishu_stream_update(const char *message_id,
                                const char *content_md);

#ifdef __cplusplus
}
#endif

#endif /* FEISHU_CHANNEL_H */
