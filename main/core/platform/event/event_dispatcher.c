#include "event_dispatcher.h"
#include "event_bus.h"
#include "os/os.h"
#include "log.h"
#include "mongoose.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "dispatcher";

#define MAX_HANDLERS 8

typedef struct {
    conn_type_t conn_type;
    event_dispatcher_handler_t handler;
    void *user_data;
} handler_entry_t;

struct event_dispatcher {
    handler_entry_t handlers[MAX_HANDLERS];
    size_t handler_count;
    size_t worker_count;
    mimi_task_handle_t *worker_threads;
    volatile bool running;
    void (*wakeup_fn)(void *arg);
    void *wakeup_arg;
    event_bus_t *event_bus;
};

static event_dispatcher_t *s_dispatcher = NULL;

static event_dispatcher_handler_t find_handler(event_dispatcher_t *disp, conn_type_t conn_type)
{
    for (size_t i = 0; i < disp->handler_count; i++) {
        if (disp->handlers[i].conn_type == conn_type) {
            return disp->handlers[i].handler;
        }
    }
    return NULL;
}

static void *find_handler_user_data(event_dispatcher_t *disp, conn_type_t conn_type)
{
    for (size_t i = 0; i < disp->handler_count; i++) {
        if (disp->handlers[i].conn_type == conn_type) {
            return disp->handlers[i].user_data;
        }
    }
    return NULL;
}

static void worker_thread_fn(void *arg)
{
    event_dispatcher_t *disp = (event_dispatcher_t *)arg;
    mimi_queue_t *recv_queue = event_bus_get_recv_queue(disp->event_bus);
    event_msg_t msg;
    
    MIMI_LOGD(TAG, "Worker thread started");
    
    while (disp->running) {
        mimi_err_t err = mimi_queue_recv(recv_queue, &msg, 100);
        if (err == MIMI_ERR_TIMEOUT || err == MIMI_ERR_WOULD_BLOCK) {
            continue;
        }
        
        if (err != MIMI_OK) {
            continue;
        }
        
        event_dispatcher_handler_t handler = find_handler(disp, msg.conn_type);
        if (handler) {
            handler(disp, &msg, find_handler_user_data(disp, msg.conn_type));
        } else {
            MIMI_LOGW(TAG, "No handler for conn_type=%d", msg.conn_type);
            if (msg.buf) {
                io_buf_unref(msg.buf);
            }
        }
    }
    
    MIMI_LOGI(TAG, "Worker thread exiting");
}

event_dispatcher_t *event_dispatcher_create(size_t worker_count, event_bus_t *event_bus)
{
    if (worker_count == 0) {
        worker_count = 2;
    }
    
    event_dispatcher_t *disp = (event_dispatcher_t *)calloc(1, sizeof(event_dispatcher_t));
    if (!disp) {
        return NULL;
    }
    
    disp->worker_count = worker_count;
    disp->worker_threads = (mimi_task_handle_t *)calloc(worker_count, sizeof(mimi_task_handle_t));
    if (!disp->worker_threads) {
        free(disp);
        return NULL;
    }
    
    disp->running = false;
    disp->handler_count = 0;
    disp->wakeup_fn = NULL;
    disp->wakeup_arg = NULL;
    disp->event_bus = event_bus;
    
    return disp;
}

void event_dispatcher_destroy(event_dispatcher_t *disp)
{
    if (!disp) {
        return;
    }
    
    event_dispatcher_stop(disp);
    
    if (disp->worker_threads) {
        free(disp->worker_threads);
        disp->worker_threads = NULL;
    }
    
    free(disp);
}

int event_dispatcher_register_handler(event_dispatcher_t *disp,
                                      conn_type_t conn_type,
                                      event_dispatcher_handler_t handler,
                                      void *user_data)
{
    if (!disp || !handler) {
        return -1;
    }
    
    if (disp->handler_count >= MAX_HANDLERS) {
        MIMI_LOGE(TAG, "Too many handlers registered");
        return -1;
    }
    
    for (size_t i = 0; i < disp->handler_count; i++) {
        if (disp->handlers[i].conn_type == conn_type) {
            disp->handlers[i].handler = handler;
            disp->handlers[i].user_data = user_data;
            return 0;
        }
    }
    
    disp->handlers[disp->handler_count].conn_type = conn_type;
    disp->handlers[disp->handler_count].handler = handler;
    disp->handlers[disp->handler_count].user_data = user_data;
    disp->handler_count++;
    
    MIMI_LOGD(TAG, "Registered handler for conn_type=%d", conn_type);
    return 0;
}

mimi_err_t event_dispatcher_start(event_dispatcher_t *disp)
{
    if (!disp) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (disp->running) {
        return MIMI_OK;
    }
    
    disp->running = true;
    
    char name[32];
    for (size_t i = 0; i < disp->worker_count; i++) {
        snprintf(name, sizeof(name), "worker_%zu", i);
        mimi_err_t err = mimi_task_create(name, worker_thread_fn, disp, 0, 0, &disp->worker_threads[i]);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to create worker thread %zu", i);
            disp->running = false;
            return err;
        }
    }
    
    MIMI_LOGD(TAG, "Dispatcher started with %zu workers", disp->worker_count);
    return MIMI_OK;
}

mimi_err_t event_dispatcher_stop(event_dispatcher_t *disp)
{
    if (!disp) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (!disp->running) {
        return MIMI_OK;
    }
    
    disp->running = false;
    
    for (size_t i = 0; i < disp->worker_count; i++) {
        if (disp->worker_threads[i]) {
            mimi_task_delete(disp->worker_threads[i]);
            disp->worker_threads[i] = NULL;
        }
    }
    
    MIMI_LOGI(TAG, "Dispatcher stopped");
    return MIMI_OK;
}

event_dispatcher_t *event_dispatcher_get_global(void)
{
    return s_dispatcher;
}

void event_dispatcher_set_global(event_dispatcher_t *disp)
{
    s_dispatcher = disp;
}

void event_dispatcher_drain_send(event_dispatcher_t *disp)
{
    if (!disp || !disp->event_bus) {
        return;
    }
    
    mimi_queue_t *send_queue = event_bus_get_send_queue(disp->event_bus);
    if (!send_queue) {
        return;
    }
    
    event_msg_t msg;
    int processed = 0;
    
    while (mimi_queue_try_recv(send_queue, &msg) == MIMI_OK) {
        processed++;
        
        switch (msg.type) {
            case EVENT_SEND: {
                struct mg_connection *c = (struct mg_connection *)msg.conn_id;
                
                if (c && msg.buf) {
                    int op = WEBSOCKET_OP_TEXT;
                    if ((msg.flags & EVENT_FLAG_BINARY) != 0) {
                        op = WEBSOCKET_OP_BINARY;
                    }
                    mg_ws_send(c, (const char *)msg.buf->base, msg.buf->len, op);
                }
                
                if (msg.buf) {
                    io_buf_unref(msg.buf);
                }
                break;
            }
            
            case EVENT_CLOSE: {
                struct mg_connection *c = (struct mg_connection *)msg.conn_id;
                if (c) {
                    mg_close_conn(c);
                }
                if (msg.buf) {
                    io_buf_unref(msg.buf);
                }
                break;
            }

            case EVENT_CALL: {
                /* Execute callback in reactor/event-loop thread */
                /* Defined in event_bus.c; keep a local mirror to avoid leaking internal headers. */
                typedef struct {
                    event_bus_call_fn_t fn;
                    void *arg;
                } event_bus_call_msg_t_local;
                event_bus_call_msg_t_local *c = (event_bus_call_msg_t_local *)ID_TO_CONN(msg.conn_id);
                if (c && c->fn) c->fn(c->arg);
                free(c);
                if (msg.buf) io_buf_unref(msg.buf);
                break;
            }
            
            default:
                if (msg.buf) {
                    io_buf_unref(msg.buf);
                }
                break;
        }
    }
    
    (void)processed;
}

void event_dispatcher_set_wakeup(event_dispatcher_t *disp,
                                 void (*fn)(void *arg),
                                 void *arg)
{
    if (disp) {
        disp->wakeup_fn = fn;
        disp->wakeup_arg = arg;
    }
}
