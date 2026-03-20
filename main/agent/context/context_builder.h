#pragma once

#include "mimi_err.h"
#include <stddef.h>
#include <stdbool.h>

mimi_err_t context_build_system_prompt(char *buf, size_t size);

/**
 * Check if this is the first run and user needs to configure personalization.
 * Returns true if template files (SOUL.md, USER.md) are still unmodified.
 */
bool context_needs_first_run_setup(void);

