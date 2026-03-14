/**
 * Cross-platform CLI library with multi-terminal support
 */

#ifndef CLI_H
#define CLI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque terminal handle */
typedef struct cli_terminal cli_terminal_t;

/* Terminal types */
typedef enum {
    CLI_TERMINAL_STDIN,      /* Standard input */
    CLI_TERMINAL_WEBSOCKET,  /* WebSocket connection */
    CLI_TERMINAL_UART,       /* Serial port */
    CLI_TERMINAL_TCP,        /* TCP socket */
    CLI_TERMINAL_CUSTOM      /* User-defined */
} cli_terminal_type_t;

/* Callbacks */
typedef void (*cli_char_input_cb_t)(void *user_data, char c);
typedef void (*cli_output_cb_t)(void *user_data, const char *str);
typedef void (*cli_execute_cb_t)(const char *line, void *user_data);
typedef int (*cli_complete_cb_t)(const char *prefix, char **matches, int max_matches, void *user_data);
typedef const char* (*cli_get_prompt_cb_t)(void *user_data);

/**
 * Initialize CLI system
 */
void cli_init(cli_execute_cb_t exec_cb, cli_complete_cb_t complete_cb, 
              cli_get_prompt_cb_t prompt_cb);

/**
 * Create a new terminal instance
 * @param type Terminal type
 * @param user_data User data passed to callbacks
 * @param input_cb Called when input is available (can be NULL for stdin)
 * @param output_cb Called to output text (required)
 * @return Terminal handle or NULL on error
 */
cli_terminal_t* cli_terminal_create(cli_terminal_type_t type, void *user_data,
                                    cli_char_input_cb_t input_cb,
                                    cli_output_cb_t output_cb);

/**
 * Destroy terminal instance
 */
void cli_terminal_destroy(cli_terminal_t *term);

/**
 * Feed character to terminal (called from external input source like WebSocket)
 */
void cli_terminal_feed_char(cli_terminal_t *term, char c);

/**
 * Feed string to terminal
 */
void cli_terminal_feed_string(cli_terminal_t *term, const char *str);

/**
 * Print prompt for terminal
 */
void cli_terminal_print_prompt(cli_terminal_t *term);

/**
 * Get current line content
 */
const char* cli_terminal_get_line(cli_terminal_t *term);

/**
 * Poll for input and process (call periodically)
 */
void cli_poll(void);

/**
 * Output from callback context (uses current active terminal)
 */
void cli_output(const char *str);
void cli_output_ln(const char *str);

/**
 * Run CLI main loop (blocking, for stdin-only mode)
 */
void cli_run(void);

/**
 * Stop CLI main loop
 */
void cli_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_H */
