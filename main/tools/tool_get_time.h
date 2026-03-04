#pragma once

#include "platform/mimi_err.h"
#include <stddef.h>

/**
 * Execute get_current_time tool.
 * Fetches current time via HTTP Date header, sets system clock, returns time string.
 */
mimi_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size);
