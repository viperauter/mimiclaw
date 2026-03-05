#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stddef.h>

/**
 * Read a file from storage. With session_ctx, relative paths resolve to session workspace.
 */
mimi_err_t tool_read_file_execute(const char *input_json, char *output, size_t output_size,
                                  const mimi_session_ctx_t *session_ctx);

/**
 * Write/overwrite a file. With session_ctx, relative paths resolve to session workspace.
 */
mimi_err_t tool_write_file_execute(const char *input_json, char *output, size_t output_size,
                                   const mimi_session_ctx_t *session_ctx);

/**
 * Find-and-replace edit a file. With session_ctx, relative paths resolve to session workspace.
 */
mimi_err_t tool_edit_file_execute(const char *input_json, char *output, size_t output_size,
                                  const mimi_session_ctx_t *session_ctx);

/**
 * List files, optionally filtered by prefix. With session_ctx, lists session workspace.
 */
mimi_err_t tool_list_dir_execute(const char *input_json, char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx);
