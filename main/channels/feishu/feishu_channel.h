/**
 * Feishu (Lark) Channel Interface
 *
 * Provides Feishu Bot integration via HTTP Webhook
 */

#ifndef FEISHU_CHANNEL_H
#define FEISHU_CHANNEL_H

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

#ifdef __cplusplus
}
#endif

#endif /* FEISHU_CHANNEL_H */
