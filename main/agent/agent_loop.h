#pragma once

#include "platform/mimi_err.h"

/**
 * Initialize the agent loop.
 */
mimi_err_t agent_loop_init(void);

/**
 * Start the agent loop task (runs on Core 1).
 * Consumes from inbound queue, calls Claude API, pushes to outbound queue.
 */
mimi_err_t agent_loop_start(void);
