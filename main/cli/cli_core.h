#pragma once

#include <stddef.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cli_write_fn_t)(void *ctx, const char *text);

typedef struct {
    cli_write_fn_t write;
    void *ctx;
} cli_io_t;

/* Command function type */
typedef mimi_err_t (*cli_cmd_fn_t)(int argc, char **argv, const cli_io_t *io);

/* Register built-in commands. Safe to call multiple times. */
mimi_err_t cli_core_init(void);

/* Execute one command line (newline optional). */
mimi_err_t cli_core_execute_line(const char *line, const cli_io_t *io);

/* Register a new CLI command dynamically */
mimi_err_t cli_register_cmd(const char *name, const char *help, cli_cmd_fn_t fn);

/* Get command completion matches for a given prefix
 * 
 * @param prefix The partial command to complete
 * @param out_matches Array to store matching command names (should be pre-allocated)
 * @param max_matches Maximum number of matches to return
 * @return Number of matches found (0 if no matches)
 */
int cli_get_completions(const char *prefix, char **out_matches, int max_matches);

#ifdef __cplusplus
}
#endif

