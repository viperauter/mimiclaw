/**
 * @file app.h
 * @brief Application layer interface - platform-agnostic application core
 *
 * This layer provides the application-level abstraction that is independent
 * of the underlying platform (POSIX, Windows, ESP32, baremetal, etc.).
 * It manages all application subsystems including message bus, channels,
 * agents, tools, and the runtime event loop.
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the application layer
 *
 * Initializes all application subsystems including:
 * - VFS and workspace
 * - Configuration
 * - Message bus
 * - Runtime (event loop)
 * - Command system
 * - Channel system
 * - Memory, skills, tools
 * - Agent loop
 *
 * @param config_path Path to configuration file (can be NULL for default)
 * @param enable_logs Enable logging if true
 * @param log_level Log level string (error, warn, info, debug)
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t app_init(const char *config_path,
                    bool enable_logs,
                    const char *log_level,
                    bool gateway_mode,
                    const char *log_file_path);

/**
 * @brief Start the application
 *
 * Starts all running services and tasks that were initialized in app_init().
 * This includes the agent loop, outbound dispatch task, cron service, and
 * heartbeat service.
 *
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t app_start(void);

/**
 * @brief Run the application (main event loop)
 *
 * Enters the main event loop and blocks until the application requests exit.
 * Upon return, the application has stopped.
 *
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t app_run(void);

/**
 * @brief Stop the application
 *
 * Signals all services and tasks to stop gracefully.
 */
void app_stop(void);

/**
 * @brief Destroy the application
 *
 * Releases all resources allocated during app_init().
 * Should be called after app_stop() returns.
 */
void app_destroy(void);

#ifdef __cplusplus
}
#endif
