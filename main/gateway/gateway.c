/**
 * Gateway Implementation
 *
 * Common gateway utility functions
 */

#include "gateway/gateway.h"
#include "log.h"
#include <string.h>

static const char *TAG = "gateway";

mimi_err_t gateway_init(gateway_t *gw, const gateway_config_t *cfg)
{
    if (!gw) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (gw->is_initialized) {
        MIMI_LOGW(TAG, "Gateway '%s' already initialized", gw->name);
        return MIMI_OK;
    }
    
    if (gw->init) {
        mimi_err_t err = gw->init(gw, cfg);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to initialize gateway '%s': %d", gw->name, err);
            return err;
        }
    }
    
    gw->is_initialized = true;
    MIMI_LOGI(TAG, "Gateway '%s' initialized", gw->name);
    return MIMI_OK;
}

mimi_err_t gateway_start(gateway_t *gw)
{
    if (!gw) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (!gw->is_initialized) {
        MIMI_LOGE(TAG, "Gateway '%s' not initialized", gw->name);
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (gw->is_started) {
        MIMI_LOGW(TAG, "Gateway '%s' already started", gw->name);
        return MIMI_OK;
    }
    
    if (gw->start) {
        mimi_err_t err = gw->start(gw);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to start gateway '%s': %d", gw->name, err);
            return err;
        }
    }
    
    gw->is_started = true;
    MIMI_LOGI(TAG, "Gateway '%s' started", gw->name);
    return MIMI_OK;
}

mimi_err_t gateway_stop(gateway_t *gw)
{
    if (!gw) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (!gw->is_started) {
        return MIMI_OK;
    }
    
    if (gw->stop) {
        gw->stop(gw);
    }
    
    gw->is_started = false;
    MIMI_LOGI(TAG, "Gateway '%s' stopped", gw->name);
    return MIMI_OK;
}

void gateway_destroy(gateway_t *gw)
{
    if (!gw) {
        return;
    }
    
    gateway_stop(gw);
    
    if (gw->destroy) {
        gw->destroy(gw);
    }
    
    gw->is_initialized = false;
    MIMI_LOGI(TAG, "Gateway '%s' destroyed", gw->name);
}

mimi_err_t gateway_send(gateway_t *gw, const char *session_id, const char *content)
{
    if (!gw || !session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (!gw->is_initialized || !gw->is_started) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (gw->send) {
        return gw->send(gw, session_id, content);
    }
    
    return MIMI_ERR_NOT_SUPPORTED;
}

void gateway_set_on_message(gateway_t *gw, gateway_on_message_cb_t cb, void *user_data)
{
    if (!gw) {
        return;
    }
    
    gw->on_message_cb = cb;
    gw->callback_user_data = user_data;
    
    if (gw->set_on_message) {
        gw->set_on_message(gw, cb, user_data);
    }
}

void gateway_set_on_connect(gateway_t *gw, gateway_on_connect_cb_t cb, void *user_data)
{
    if (!gw) {
        return;
    }
    
    gw->on_connect_cb = cb;
    gw->callback_user_data = user_data;
    
    if (gw->set_on_connect) {
        gw->set_on_connect(gw, cb, user_data);
    }
}

void gateway_set_on_disconnect(gateway_t *gw, gateway_on_disconnect_cb_t cb, void *user_data)
{
    if (!gw) {
        return;
    }
    
    gw->on_disconnect_cb = cb;
    gw->callback_user_data = user_data;
    
    if (gw->set_on_disconnect) {
        gw->set_on_disconnect(gw, cb, user_data);
    }
}
