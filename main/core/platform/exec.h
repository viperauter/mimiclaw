#pragma once

#include "mimi_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MIMI_EXEC_ENV_HOST = 0,
    MIMI_EXEC_ENV_SANDBOX = 1,
} mimi_exec_env_t;

typedef struct {
    const char *command;
    const char *cwd;
    const char *workspace_root; /* Used by sandbox runner to bind-mount workspace */
    int timeout_ms;
    size_t max_output_bytes;
    bool merge_stderr;
    mimi_exec_env_t env;
} mimi_exec_spec_t;

typedef struct {
    int exit_code;
    int term_signal;
    bool timed_out;
    bool truncated;
    size_t output_len;
} mimi_exec_result_t;

mimi_err_t mimi_exec_run(const mimi_exec_spec_t *spec,
                         char *output, size_t output_size,
                         mimi_exec_result_t *result);
