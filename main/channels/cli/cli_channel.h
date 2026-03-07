/**
 * CLI Channel Header
 *
 * CLI Channel implementation that wraps the existing CLI terminal
 * and adapts it to the Channel interface.
 */

#ifndef CLI_CHANNEL_H
#define CLI_CHANNEL_H

#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CLI Channel instance
 * Declared as extern, defined in cli_channel.c
 */
extern channel_t g_cli_channel;

/**
 * Initialize CLI Channel
 * Must be called before registering with Channel Manager
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t cli_channel_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_CHANNEL_H */
