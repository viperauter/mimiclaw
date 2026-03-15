#pragma once

#include "mimi_err.h"
#include "bus/message_bus.h"

mimi_err_t agent_async_loop_init(void);
mimi_err_t agent_async_loop_start(void);

/**
 * Stop the async agent loop task.
 */
void agent_async_loop_stop(void);

/* Unified API aliases for async mode */
#define agent_loop_init     agent_async_loop_init
#define agent_loop_start    agent_async_loop_start
#define agent_loop_stop     agent_async_loop_stop
