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
 * Initialize channel system
 * Registers and initializes all built-in channels (but does not start them)
 * @return MIMI_OK on success
 */
mimi_err_t channel_system_init(void);

/**
 * Start channel system
 * Starts all registered channels
 * @return MIMI_OK on success
 */
mimi_err_t channel_system_start(void);

/**
 * Stop channel system
 * Stops all registered channels
 */
void channel_system_stop(void);

/**
 * Auto-initialize channel manager and register all built-in channels
 * This is a convenience function that:
 * 1. Initializes channel system
 * 2. Starts all registered channels
 * @return MIMI_OK on success, error code otherwise
 * @deprecated Use channel_system_init() + channel_system_start() instead
 */
mimi_err_t channel_system_auto_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_MANAGER_H */
