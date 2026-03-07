/**
 * WebSocket Channel Interface
 *
 * Wraps the existing ws_server implementation to conform to the Channel interface.
 * This allows WebSocket to be managed by the Channel Manager alongside other channels.
 */

#pragma once

#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Global WebSocket channel instance.
 * Register this with channel_register() after initialization.
 */
extern channel_t g_websocket_channel;

/**
 * Initialize WebSocket Channel module.
 * Must be called before registering with Channel Manager.
 *
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t ws_channel_init(void);

/**
 * Set callback for incoming messages.
 * Called when a message is received from WebSocket.
 *
 * @param ch     Channel instance
 * @param cb     Callback function
 * @param user_data User data passed to callback
 */
void ws_channel_set_on_message(channel_t *ch,
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
void ws_channel_set_on_connect(channel_t *ch,
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
void ws_channel_set_on_disconnect(channel_t *ch,
                                   void (*cb)(channel_t *, const char *session_id,
                                              void *user_data),
                                   void *user_data);

#ifdef __cplusplus
}
#endif
