#pragma once

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include <stddef.h>

/**
 * Add a scheduled cron job.
 * Input JSON: { name, schedule_type ("every"/"at"), interval_s, at_epoch, message, channel?, chat_id? }
 */
mimi_err_t tool_cron_add_execute(const char *input_json, char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx);

/**
 * List all scheduled cron jobs.
 * Input JSON: {} (no required fields)
 */
mimi_err_t tool_cron_list_execute(const char *input_json, char *output, size_t output_size,
                                  const mimi_session_ctx_t *session_ctx);

/**
 * Remove a scheduled cron job by ID.
 * Input JSON: { job_id }
 */
mimi_err_t tool_cron_remove_execute(const char *input_json, char *output, size_t output_size,
                                    const mimi_session_ctx_t *session_ctx);
