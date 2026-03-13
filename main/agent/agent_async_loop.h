#pragma once

#include "mimi_err.h"
#include "bus/message_bus.h"

mimi_err_t agent_async_loop_init(void);
mimi_err_t agent_async_loop_start(void);

/**
 * Stop the async agent loop task.
 */
void agent_async_loop_stop(void);
