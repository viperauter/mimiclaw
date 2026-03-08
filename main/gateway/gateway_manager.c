/**
 * Gateway Manager Implementation
 */

#include "gateway/gateway_manager.h"
#include "platform/log.h"
#include <string.h>

static const char *TAG = "gateway_mgr";

/* Gateway registry */
static struct {
    gateway_t *gateways[GATEWAY_MAX_COUNT];
    int count;
    bool initialized;
} g_manager = {0};

mimi_err_t gateway_manager_init(void)
{
    if (g_manager.initialized) {
        MIMI_LOGW(TAG, "Gateway manager already initialized");
        return MIMI_OK;
    }
    
    memset(&g_manager, 0, sizeof(g_manager));
    g_manager.initialized = true;
    
    MIMI_LOGI(TAG, "Gateway manager initialized (max=%d)", GATEWAY_MAX_COUNT);
    return MIMI_OK;
}

mimi_err_t gateway_manager_register(gateway_t *gw)
{
    if (!g_manager.initialized) {
        MIMI_LOGE(TAG, "Gateway manager not initialized");
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!gw || !gw->name[0]) {
        MIMI_LOGE(TAG, "Invalid gateway");
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Check if already registered */
    if (gateway_manager_find(gw->name) != NULL) {
        MIMI_LOGW(TAG, "Gateway '%s' already registered", gw->name);
        return MIMI_ERR_ALREADY_EXISTS;
    }
    
    /* Check capacity */
    if (g_manager.count >= GATEWAY_MAX_COUNT) {
        MIMI_LOGE(TAG, "Gateway registry full (%d/%d)", g_manager.count, GATEWAY_MAX_COUNT);
        return MIMI_ERR_NO_MEM;
    }
    
    /* Register */
    g_manager.gateways[g_manager.count++] = gw;
    
    MIMI_LOGI(TAG, "Gateway '%s' registered (%d/%d)", gw->name, g_manager.count, GATEWAY_MAX_COUNT);
    return MIMI_OK;
}

mimi_err_t gateway_manager_unregister(const char *name)
{
    if (!g_manager.initialized || !name) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    for (int i = 0; i < g_manager.count; i++) {
        if (strcmp(g_manager.gateways[i]->name, name) == 0) {
            /* Stop if running */
            if (g_manager.gateways[i]->is_started) {
                gateway_stop(g_manager.gateways[i]);
            }
            
            /* Shift remaining */
            for (int j = i; j < g_manager.count - 1; j++) {
                g_manager.gateways[j] = g_manager.gateways[j + 1];
            }
            g_manager.count--;
            
            MIMI_LOGI(TAG, "Gateway '%s' unregistered", name);
            return MIMI_OK;
        }
    }
    
    return MIMI_ERR_NOT_FOUND;
}

gateway_t* gateway_manager_find(const char *name)
{
    if (!g_manager.initialized || !name) {
        return NULL;
    }
    
    for (int i = 0; i < g_manager.count; i++) {
        if (strcmp(g_manager.gateways[i]->name, name) == 0) {
            return g_manager.gateways[i];
        }
    }
    
    return NULL;
}

mimi_err_t gateway_manager_start_all(void)
{
    if (!g_manager.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    MIMI_LOGI(TAG, "Starting all gateways (%d)...", g_manager.count);
    
    for (int i = 0; i < g_manager.count; i++) {
        gateway_t *gw = g_manager.gateways[i];
        
        if (!gw->is_initialized) {
            MIMI_LOGW(TAG, "Gateway '%s' not initialized, skipping", gw->name);
            continue;
        }
        
        if (gw->is_started) {
            MIMI_LOGW(TAG, "Gateway '%s' already started", gw->name);
            continue;
        }
        
        mimi_err_t err = gateway_start(gw);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to start gateway '%s': %d", gw->name, err);
            /* Continue with others */
        }
    }
    
    MIMI_LOGI(TAG, "All gateways started");
    return MIMI_OK;
}

void gateway_manager_stop_all(void)
{
    if (!g_manager.initialized) {
        return;
    }
    
    MIMI_LOGI(TAG, "Stopping all gateways...");
    
    for (int i = 0; i < g_manager.count; i++) {
        gateway_t *gw = g_manager.gateways[i];
        
        if (gw->is_started) {
            gateway_stop(gw);
        }
    }
    
    MIMI_LOGI(TAG, "All gateways stopped");
}

void gateway_manager_destroy_all(void)
{
    if (!g_manager.initialized) {
        return;
    }
    
    MIMI_LOGI(TAG, "Destroying all gateways...");
    
    gateway_manager_stop_all();
    
    for (int i = 0; i < g_manager.count; i++) {
        gateway_destroy(g_manager.gateways[i]);
    }
    
    g_manager.count = 0;
    g_manager.initialized = false;
    
    MIMI_LOGI(TAG, "All gateways destroyed");
}

int gateway_manager_count(void)
{
    return g_manager.initialized ? g_manager.count : 0;
}

void gateway_manager_foreach(void (*callback)(gateway_t *gw, void *user_data), 
                             void *user_data)
{
    if (!g_manager.initialized || !callback) {
        return;
    }
    
    for (int i = 0; i < g_manager.count; i++) {
        callback(g_manager.gateways[i], user_data);
    }
}
