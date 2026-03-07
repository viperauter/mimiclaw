/**
 * Channel Manager
 * 
 * Manages registration, startup, shutdown, and message distribution for all channels
 */

#ifndef CHANNEL_MANAGER_H
#define CHANNEL_MANAGER_H

#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Channel manager configuration
 */
typedef struct {
    int max_channels;           /* Maximum number of channels, default CHANNEL_MAX_COUNT */
    bool auto_start;            /* Auto-start after registration */
} channel_manager_config_t;

/**
 * Initialize channel manager with configuration
 * @param cfg Configuration, NULL for default configuration
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t channel_manager_init_with_config(const channel_manager_config_t *cfg);

/**
 * Get channel manager configuration
 * @return Current configuration
 */
const channel_manager_config_t* channel_manager_get_config(void);

/**
 * Check if channel manager is initialized
 * @return true if initialized, false otherwise
 */
bool channel_manager_is_initialized(void);

/**
 * Auto-initialize channel manager and register all built-in channels
 * This is a convenience function that:
 * 1. Initializes channel manager
 * 2. Initializes and registers CLI Channel
 * 3. Initializes and registers Telegram Channel (optional)
 * 4. Initializes and registers WebSocket Channel (optional)
 * 5. Starts all registered channels
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t channel_system_auto_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_MANAGER_H */
