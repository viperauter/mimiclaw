#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Platform Runtime / Event Loop Manager
 *
 * The runtime provides a platform-independent event loop that runs in its
 * own thread. It manages timers, network I/O (via Mongoose), and provides
 * a centralized execution context for the application.
 *
 * Lifecycle:
 *   init() -> start() [creates thread] -> stop() [graceful shutdown] -> deinit()
 *
 * Thread Safety:
 *   All public functions are thread-safe unless otherwise noted.
 * ------------------------------------------------------------------------- */

typedef enum {
    RUNTIME_STATE_IDLE = 0,
    RUNTIME_STATE_RUNNING,
    RUNTIME_STATE_STOPPING,
    RUNTIME_STATE_STOPPED
} mimi_runtime_state_t;

/**
 * Initialize the runtime.
 * Sets up internal data structures but does not start the event loop.
 * Must be called before any other runtime functions.
 * @return MIMI_OK on success
 */
mimi_err_t mimi_runtime_init(void);

/**
 * Start the runtime.
 * Creates a dedicated thread to run the event loop.
 * Returns immediately without blocking.
 * @return MIMI_OK on success, MIMI_ERR_INVALID_STATE if already running
 */
mimi_err_t mimi_runtime_start(void);

/**
 * Stop the runtime.
 * Signals the event loop to exit gracefully and waits for the thread to finish.
 * Blocks until the runtime has fully stopped.
 */
void mimi_runtime_stop(void);

/**
 * Deinitialize the runtime.
 * Releases all resources. Must be called after stop().
 * After deinit, init() can be called again to restart.
 */
void mimi_runtime_deinit(void);

/**
 * Get current runtime state.
 * @return Current state of the runtime
 */
mimi_runtime_state_t mimi_runtime_get_state(void);

/**
 * Check if runtime is running.
 * @return true if state is RUNNING
 */
bool mimi_runtime_is_running(void);

/**
 * Request the runtime to exit gracefully.
 * This is typically called from signal handlers or shutdown callbacks.
 * The runtime will stop at the next opportunity.
 */
void mimi_runtime_request_exit(void);

/**
 * Check if runtime exit has been requested.
 * @return true if exit has been requested
 */
bool mimi_runtime_should_exit(void);

/**
 * Get the underlying event loop object.
 * Can be used by platform-specific modules to register listeners.
 * The returned pointer is opaque; cast to appropriate type as needed.
 * @return Pointer to internal event loop structure
 */
void *mimi_runtime_get_event_loop(void);

/**
 * Get the event dispatcher.
 * @return Pointer to event dispatcher
 */
void *mimi_runtime_get_dispatcher(void);

#ifdef __cplusplus
}
#endif
