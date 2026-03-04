#pragma once

#include <stddef.h>
#include "platform/mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cli_write_fn_t)(void *ctx, const char *text);

typedef struct {
    cli_write_fn_t write;
    void *ctx;
} cli_io_t;

/* Register built-in commands. Safe to call multiple times. */
mimi_err_t cli_core_init(void);

/* Execute one command line (newline optional). */
mimi_err_t cli_core_execute_line(const char *line, const cli_io_t *io);

#ifdef __cplusplus
}
#endif

