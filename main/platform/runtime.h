#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Platform runtime / main event loop
 *
 * This layer wraps the "whole runtime" for a given platform: event loop,
 * timers integration, and any global polling mechanism. It hides whether
 * the implementation is based on an OS, a library like Mongoose, or a
 * bare-metal while(1) loop.
 * ------------------------------------------------------------------------- */

/**
 * Initialize the platform runtime. Should be called after core subsystems
 * (message_bus, memory, tools, etc.) are initialized but before entering
 * the main event loop.
 */
mimi_err_t mimi_runtime_init(void);

/**
 * Enter the main event loop. On most platforms this does not return under
 * normal conditions. Returns an error only on fatal failure.
 */
mimi_err_t mimi_runtime_run(void);

/**
 * Request the runtime to exit gracefully.
 * This sets an internal flag that the runtime will check periodically.
 */
void mimi_runtime_request_exit(void);

/**
 * Check if exit has been requested.
 * @return true if exit has been requested, false otherwise
 */
bool mimi_runtime_should_exit(void);

/**
 * Optional: retrieve the underlying event loop object for platform-specific
 * modules that need to attach listeners (e.g. WebSocket server on POSIX).
 * The returned pointer is opaque to most of the application; platform
 * specific code may cast it to the appropriate type.
 */
void *mimi_runtime_get_event_loop(void);

#ifdef __cplusplus
}
#endif

