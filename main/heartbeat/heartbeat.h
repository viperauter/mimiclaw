#pragma once

#include "platform/mimi_err.h"
#include <stdbool.h>

/**
 * Initialize the heartbeat service (logs ready state).
 */
mimi_err_t heartbeat_init(void);

/**
 * Start the heartbeat timer. Checks HEARTBEAT.md periodically
 * and sends a prompt to the agent if actionable tasks are found.
 */
mimi_err_t heartbeat_start(void);

/**
 * Stop and delete the heartbeat timer.
 */
void heartbeat_stop(void);

/**
 * Manually trigger a heartbeat check (for CLI testing).
 * Returns true if the agent was prompted, false if no tasks found.
 */
bool heartbeat_trigger(void);

/* POSIX: provide mg_mgr so heartbeat can schedule a timer. */
struct mg_mgr;
void heartbeat_set_mgr(struct mg_mgr *mgr);
