/**
 * WeChat Channel Interface
 *
 * WeChat iLink Bot Channel implementation using HTTP long-polling.
 * This allows WeChat to be managed by the Channel Manager alongside other channels.
 */

#pragma once

#include <stddef.h>
#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Global WeChat channel instance.
 * Register this with channel_register() after initialization.
 */
extern channel_t g_wechat_channel;

/**
 * Initialize WeChat Channel module.
 * Must be called before registering with Channel Manager.
 *
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t wechat_channel_init(void);

/**
 * Set callback for incoming messages.
 * Called when a message is received from WeChat.
 *
 * @param ch     Channel instance
 * @param cb     Callback function
 * @param user_data User data passed to callback
 */
void wechat_channel_set_on_message(channel_t *ch,
                                   void (*cb)(channel_t *, const char *session_id,
                                              const char *content, void *user_data),
                                   void *user_data);

/**
 * Set callback for new connections.
 *
 * @param ch     Channel instance
 * @param cb     Callback function
 * @param user_data User data passed to callback
 */
void wechat_channel_set_on_connect(channel_t *ch,
                                   void (*cb)(channel_t *, const char *session_id,
                                              void *user_data),
                                   void *user_data);

/**
 * Set callback for disconnections.
 *
 * @param ch     Channel instance
 * @param cb     Callback function
 * @param user_data User data passed to callback
 */
void wechat_channel_set_on_disconnect(channel_t *ch,
                                      void (*cb)(channel_t *, const char *session_id,
                                                 void *user_data),
                                      void *user_data);

/**
 * Manually set WeChat bot token (alternative to config).
 * Can be used to update token at runtime.
 *
 * @param token  Bot token obtained from QR code login
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t wechat_channel_set_token(const char *token);

/**
 * Trigger QR code login flow.
 * Returns QR code URL that user can scan with WeChat.
 *
 * @param qr_url  Buffer to store QR code URL
 * @param qr_url_len  Buffer length
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t wechat_channel_start_qr_login(char *qr_url, size_t qr_url_len);

/**
 * Check QR code login status.
 * Call this repeatedly after starting QR login to check if user has scanned.
 *
 * @param status_buffer  Buffer to store status JSON response
 * @param buffer_len     Buffer length
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t wechat_channel_check_qr_status(char *status_buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif
