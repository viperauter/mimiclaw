/**
 * QQ Channel Interface
 *
 * Provides QQ Bot integration via HTTP API or WebSocket
 */

#ifndef QQ_CHANNEL_H
#define QQ_CHANNEL_H

#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * QQ Channel instance
 */
extern channel_t g_qq_channel;

/**
 * Initialize QQ Channel module
 * Called before registering with Channel Manager
 * @return MIMI_OK on success
 */
mimi_err_t qq_channel_init(void);

/**
 * Set QQ bot credentials
 * @param app_id QQ App ID
 * @param token QQ Bot Token
 * @return MIMI_OK on success
 */
mimi_err_t qq_channel_set_credentials(const char *app_id, const char *token);

#ifdef __cplusplus
}
#endif

#endif /* QQ_CHANNEL_H */
