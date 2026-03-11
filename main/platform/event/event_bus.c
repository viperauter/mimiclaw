#include "event_bus.h"
#include "log.h"
#include "os/os.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "event_bus";

struct event_bus {
    mimi_queue_t *recv_queue;
    mimi_queue_t *send_queue;
    size_t queue_capacity;
    event_bus_wakeup_fn wakeup_fn;
    void *wakeup_arg;
};

static event_bus_t *s_global_bus = NULL;

event_bus_t *event_bus_create(size_t queue_capacity)
{
    event_bus_t *bus = (event_bus_t *)calloc(1, sizeof(event_bus_t));
    if (!bus) {
        MIMI_LOGE(TAG, "Failed to allocate event bus");
        return NULL;
    }
    
    bus->queue_capacity = queue_capacity > 0 ? queue_capacity : 64;
    
    if (mimi_queue_create(&bus->recv_queue, sizeof(event_msg_t), bus->queue_capacity) != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create recv queue");
        free(bus);
        return NULL;
    }
    
    if (mimi_queue_create(&bus->send_queue, sizeof(event_msg_t), bus->queue_capacity) != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create send queue");
        mimi_queue_destroy(bus->recv_queue);
        free(bus);
        return NULL;
    }
    
    bus->wakeup_fn = NULL;
    bus->wakeup_arg = NULL;
    
    MIMI_LOGI(TAG, "Event bus created (capacity=%zu)", bus->queue_capacity);
    return bus;
}

void event_bus_destroy(event_bus_t *bus)
{
    if (!bus) {
        return;
    }
    
    if (bus->recv_queue) {
        mimi_queue_destroy(bus->recv_queue);
        bus->recv_queue = NULL;
    }
    
    if (bus->send_queue) {
        mimi_queue_destroy(bus->send_queue);
        bus->send_queue = NULL;
    }
    
    MIMI_LOGI(TAG, "Event bus destroyed");
    free(bus);
}

mimi_queue_t *event_bus_get_recv_queue(event_bus_t *bus)
{
    return bus ? bus->recv_queue : NULL;
}

mimi_queue_t *event_bus_get_send_queue(event_bus_t *bus)
{
    return bus ? bus->send_queue : NULL;
}

int event_bus_post_recv(event_bus_t *bus,
                       event_type_t type,
                       uint64_t conn_id,
                       conn_type_t conn_type,
                       io_buf_t *buf,
                       uint64_t user_data,
                       uint32_t flags)
{
    if (!bus || !bus->recv_queue) {
        return -1;
    }
    
    event_msg_t msg = {
        .conn_id = conn_id,
        .user_data = user_data,
        .timestamp_ns = (uint64_t)mimi_time_ms() * 1000000,
        .type = type,
        .conn_type = conn_type,
        .flags = flags,
        .error_code = 0,
        .buf = buf ? io_buf_ref(buf) : NULL
    };
    
    mimi_err_t err = mimi_queue_try_send(bus->recv_queue, &msg);
    if (err != MIMI_OK) {
        if (msg.buf) {
            io_buf_unref(msg.buf);
        }
        MIMI_LOGW(TAG, "Recv queue full, dropping event type=%d", type);
        return -1;
    }
    
    return 0;
}

int event_bus_post_send(event_bus_t *bus,
                       uint64_t conn_id,
                       conn_type_t conn_type,
                       io_buf_t *buf,
                       uint32_t flags)
{
    if (!bus || !bus->send_queue) {
        return -1;
    }
    
    event_msg_t msg = {
        .conn_id = conn_id,
        .user_data = 0,
        .timestamp_ns = (uint64_t)mimi_time_ms() * 1000000,
        .type = EVENT_SEND,
        .conn_type = conn_type,
        .flags = flags,
        .error_code = 0,
        .buf = buf ? io_buf_ref(buf) : NULL
    };
    
    mimi_err_t err = mimi_queue_try_send(bus->send_queue, &msg);
    if (err != MIMI_OK) {
        if (msg.buf) {
            io_buf_unref(msg.buf);
        }
        MIMI_LOGW(TAG, "Send queue full, dropping send request");
        return -1;
    }
    
    // Wakeup reactor if needed
    if (bus->wakeup_fn) {
        bus->wakeup_fn(bus->wakeup_arg);
    }
    
    return 0;
}

int event_bus_post_close(event_bus_t *bus,
                        uint64_t conn_id,
                        conn_type_t conn_type,
                        uint32_t flags)
{
    if (!bus || !bus->send_queue) {
        return -1;
    }
    
    event_msg_t msg = {
        .conn_id = conn_id,
        .user_data = 0,
        .timestamp_ns = (uint64_t)mimi_time_ms() * 1000000,
        .type = EVENT_CLOSE,
        .conn_type = conn_type,
        .flags = flags,
        .error_code = 0,
        .buf = NULL
    };
    
    mimi_err_t err = mimi_queue_try_send(bus->send_queue, &msg);
    if (err != MIMI_OK) {
        MIMI_LOGW(TAG, "Send queue full, dropping close request");
        return -1;
    }
    
    // Wakeup reactor if needed
    if (bus->wakeup_fn) {
        bus->wakeup_fn(bus->wakeup_arg);
    }
    
    return 0;
}

int event_bus_post_error(event_bus_t *bus,
                        uint64_t conn_id,
                        conn_type_t conn_type,
                        int32_t error_code,
                        uint64_t user_data,
                        uint32_t flags)
{
    if (!bus || !bus->recv_queue) {
        return -1;
    }
    
    event_msg_t msg = {
        .conn_id = conn_id,
        .user_data = user_data,
        .timestamp_ns = (uint64_t)mimi_time_ms() * 1000000,
        .type = EVENT_ERROR,
        .conn_type = conn_type,
        .flags = flags,
        .error_code = error_code,
        .buf = NULL
    };
    
    mimi_err_t err = mimi_queue_try_send(bus->recv_queue, &msg);
    if (err != MIMI_OK) {
        MIMI_LOGW(TAG, "Recv queue full, dropping error event");
        return -1;
    }
    
    return 0;
}

void event_bus_set_wakeup(event_bus_t *bus,
                         event_bus_wakeup_fn fn,
                         void *arg)
{
    if (bus) {
        bus->wakeup_fn = fn;
        bus->wakeup_arg = arg;
    }
}

void event_bus_drain_recv(event_bus_t *bus)
{
    // This function can be implemented to process recv queue messages
    // For now, it's left as a placeholder
    (void)bus;
}

void event_bus_drain_send(event_bus_t *bus)
{
    // This function can be implemented to process send queue messages
    // For now, it's left as a placeholder
    (void)bus;
}

/* Global event bus management */
void event_bus_set_global(event_bus_t *bus)
{
    s_global_bus = bus;
}

event_bus_t *event_bus_get_global(void)
{
    return s_global_bus;
}