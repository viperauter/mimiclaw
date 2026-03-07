/**
 * Telegram Channel Interface
 *
 * Wraps the existing telegram_bot implementation to conform to the Channel interface.
 * This allows Telegram to be managed by the Channel Manager alongside other channels.
 */

#pragma once

#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Global Telegram channel instance.
 * Register this with channel_register() after initialization.
 */
extern channel_t g_telegram_channel;

/**
 * Initialize Telegram Channel module.
 * Must be called before registering with Channel Manager.
 *
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t telegram_channel_init(void);

/**
 * Set callback for incoming messages.
 * Called when a message is received from Telegram.
 *
 * @param ch     Channel instance
 * @param cb     Callback function
 * @param user_data User data passed to callback
 */
void telegram_channel_set_on_message(channel_t *ch,
                                      void (*cb)(channel_t *, const char *session_id,
                                                 const char *content, void *user_data),
                                      void *user_data);

/**
 * Set callback for new connections (new chats).
 *
 * @param ch     Channel instance
 * @param cb     Callback function
 * @param user_data User data passed to callback
 */
void telegram_channel_set_on_connect(channel_t *ch,
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
void telegram_channel_set_on_disconnect(channel_t *ch,
                                         void (*cb)(channel_t *, const char *session_id,
                                                    void *user_data),
                                         void *user_data);

/**
 * Set Telegram bot token.
 * Can be used to update token at runtime.
 *
 * @param token  Bot token from @BotFather
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t telegram_channel_set_token(const char *token);

#ifdef __cplusplus
}
#endif
