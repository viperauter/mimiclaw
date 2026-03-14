#pragma once

#include "event_bus.h"
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatcher */
typedef struct event_dispatcher event_dispatcher_t;

/* Event handler callback - gateway implements this */
typedef void (*event_dispatcher_handler_t)(event_dispatcher_t *disp, event_msg_t *msg, void *user_data);

/* Create dispatcher with worker threads */
event_dispatcher_t *event_dispatcher_create(size_t worker_count, event_bus_t *event_bus);

/* Destroy dispatcher */
void event_dispatcher_destroy(event_dispatcher_t *disp);

/* Register a handler for a connection type (for recv events) */
int event_dispatcher_register_handler(event_dispatcher_t *disp,
                                      conn_type_t conn_type,
                                      event_dispatcher_handler_t handler,
                                      void *user_data);

/* Start/stop dispatcher workers */
mimi_err_t event_dispatcher_start(event_dispatcher_t *disp);
mimi_err_t event_dispatcher_stop(event_dispatcher_t *disp);

/* Get global dispatcher instance */
event_dispatcher_t *event_dispatcher_get_global(void);

/* Set global dispatcher instance */
void event_dispatcher_set_global(event_dispatcher_t *disp);

/* Process send queue (called from event loop) */
void event_dispatcher_drain_send(event_dispatcher_t *disp);

/* Set wakeup function for event loop */
void event_dispatcher_set_wakeup(event_dispatcher_t *disp,
                                 void (*fn)(void *arg),
                                 void *arg);

#ifdef __cplusplus
}
#endif
