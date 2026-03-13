#include "message_bus.h"
#include "config.h"
#include "log.h"
#include "queue.h"
#include <string.h>

static const char *TAG = "bus";

static mimi_queue_t *s_inbound_queue;
static mimi_queue_t *s_outbound_queue;

mimi_err_t message_bus_init(void)
{
    const mimi_config_t *cfg = mimi_config_get();
    int qlen = (cfg->bus_queue_len > 0) ? cfg->bus_queue_len : 16;

    if (mimi_queue_create(&s_inbound_queue, sizeof(mimi_msg_t), (size_t)qlen) != MIMI_OK ||
        mimi_queue_create(&s_outbound_queue, sizeof(mimi_msg_t), (size_t)qlen) != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create message queues");
        return MIMI_ERR_NO_MEM;
    }

    MIMI_LOGD(TAG, "Message bus initialized (queue depth %d)", qlen);
    return MIMI_OK;
}

mimi_err_t message_bus_push_inbound(const mimi_msg_t *msg)
{
    mimi_err_t e = mimi_queue_send(s_inbound_queue, msg, 1000);
    if (e != MIMI_OK) {
        MIMI_LOGW(TAG, "Inbound queue full/blocked, dropping message");
        return e;
    }
    return MIMI_OK;
}

mimi_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms)
{
    mimi_err_t e = mimi_queue_recv(s_inbound_queue, msg, timeout_ms);
    return e;
}

mimi_err_t message_bus_push_outbound(const mimi_msg_t *msg)
{
    mimi_err_t e = mimi_queue_send(s_outbound_queue, msg, 1000);
    if (e != MIMI_OK) {
        MIMI_LOGW(TAG, "Outbound queue full/blocked, dropping message");
        return e;
    }
    return MIMI_OK;
}

mimi_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms)
{
    mimi_err_t e = mimi_queue_recv(s_outbound_queue, msg, timeout_ms);
    return e;
}
