/**
 * Generic CLI Terminal Framework (Application Layer)
 * Plugin architecture for different transport layers
 */

#ifndef CLI_TERMINAL_H
#define CLI_TERMINAL_H

#include <stddef.h>
#include <stdbool.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct app_terminal app_terminal_t;

/* Transport interface - implemented by each terminal type */
typedef struct {
    /**
     * Read data from transport
     * @param ctx Transport-specific context
     * @param buf Buffer to store read data
     * @param len Maximum bytes to read
     * @return Number of bytes read, 0 if no data, -1 on error
     */
    int (*read)(void *ctx, char *buf, int len);
    
    /**
     * Write data to transport
     * @param ctx Transport-specific context
     * @param buf Data to write
     * @param len Number of bytes to write
     * @return Number of bytes written, -1 on error
     */
    int (*write)(void *ctx, const char *buf, int len);
    
    /**
     * Check if data is available to read (non-blocking)
     * @param ctx Transport-specific context
     * @return true if data available, false otherwise
     */
    bool (*available)(void *ctx);
    
    /**
     * Close transport and cleanup resources
     * @param ctx Transport-specific context
     */
    void (*close)(void *ctx);
    
    /** Transport-specific context */
    void *ctx;
} cli_transport_t;

/* Terminal configuration */
typedef struct {
    const char *name;           /* Terminal name (e.g., "stdio", "tcp:192.168.1.1") */
    const char *channel;        /* Channel ID for session management */
    const char *chat_id;        /* Chat ID for session management */
    cli_transport_t transport;  /* Transport interface */
} app_terminal_config_t;

/**
 * Initialize CLI terminal framework
 * Must be called before creating any terminals
 */
mimi_err_t app_terminal_init(void);

/**
 * Create a new terminal instance
 * @param config Terminal configuration
 * @return Terminal handle or NULL on error
 */
app_terminal_t* app_terminal_create(const app_terminal_config_t *config);

/**
 * Destroy terminal instance
 * @param term Terminal handle
 */
void app_terminal_destroy(app_terminal_t *term);

/**
 * Poll all terminals for input
 * Call this periodically in the main loop
 */
void app_terminal_poll_all(void);

/**
 * Write output to a specific terminal
 * @param term Terminal handle
 * @param text Text to output
 */
void app_terminal_output(app_terminal_t *term, const char *text);

/**
 * Write output with newline to a specific terminal
 * @param term Terminal handle
 * @param text Text to output
 */
void app_terminal_output_ln(app_terminal_t *term, const char *text);

/**
 * Get terminal name
 * @param term Terminal handle
 * @return Terminal name
 */
const char* app_terminal_get_name(app_terminal_t *term);

/**
 * Get current number of active terminals
 * @return Number of terminals
 */
int app_terminal_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_TERMINAL_H */
