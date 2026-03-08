/**
 * QQ Channel Implementation
 *
 * Uses WebSocket Client Gateway for QQ Bot integration.
 * Provides real-time message handling via WebSocket connection.
 */

#include "channels/qq/qq_channel.h"
#include "channels/channel_manager.h"
#include "router/router.h"
#include "commands/command.h"
#include "config.h"
#include "bus/message_bus.h"
#include "gateway/websocket/ws_client_gateway.h"
#include "gateway/http/http_gateway.h"
#include "gateway/gateway_manager.h"
#include "log.h"
#include "os/os.h"
#include "mimi_time.h"

#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "qq";

/* QQ Channel private data */
typedef struct {
    bool initialized;
    bool started;
    gateway_t *ws_gateway;
    gateway_t *http_gateway;
    char app_id[64];
    char token[256];
    char session_id[64];
    volatile bool running;
} qq_channel_priv_t;

static qq_channel_priv_t s_priv = {0};

/* Forward declarations */
static bool qq_is_running_impl(channel_t *ch);

/**
 * Send message to QQ channel/user
 */
static mimi_err_t qq_send_message(const char *channel_id, const char *content)
{
    if (!s_priv.http_gateway) {
        return MIMI_ERR_INVALID_STATE;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "content", content);

    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/channels/%s/messages", channel_id);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return MIMI_ERR_NO_MEM;

    char response[4096];
    mimi_err_t err = http_gateway_post(s_priv.http_gateway, endpoint, 
                                          json, strlen(json),
                                          response, sizeof(response));
    free(json);

    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to send message: %d", err);
        return err;
    }

    return MIMI_OK;
}

/**
 * Handle incoming message from QQ
 */
static void handle_message(const char *user_id, const char *channel_id,
                            const char *content)
{
    MIMI_LOGI(TAG, "Incoming message from %s: %.40s...", user_id, content);

    /* Store channel_id for reply */
    if (channel_id) {
        strncpy(s_priv.session_id, channel_id, sizeof(s_priv.session_id) - 1);
    }

    /* Route through Input Processor */
    router_handle_qq(user_id, content);
}

/**
 * WebSocket message handler for QQ
 */
static void on_ws_message(gateway_t *gw, const char *session_id,
                         const char *content, void *user_data)
{
    (void)session_id;
    (void)user_data;
    
    if (!content) {
        return;
    }

    /* Parse QQ WebSocket message */
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        MIMI_LOGW(TAG, "Invalid WebSocket message format");
        return;
    }

    /* Handle different message types */
    cJSON *op = cJSON_GetObjectItem(root, "op");
    if (op && cJSON_IsNumber(op)) {
        int opcode = op->valueint;
        
        if (opcode == 0) {
            /* Dispatch event */
            cJSON *type = cJSON_GetObjectItem(root, "t");
            cJSON *data = cJSON_GetObjectItem(root, "d");
            
            if (type && cJSON_IsString(type) && data) {
                const char *event_type = type->valuestring;
                
                if (strcmp(event_type, "MESSAGE_CREATE") == 0) {
                    /* User message received */
                    cJSON *author = cJSON_GetObjectItem(data, "author");
                    cJSON *channel = cJSON_GetObjectItem(data, "channel_id");
                    cJSON *content = cJSON_GetObjectItem(data, "content");
                    
                    if (author && channel && content) {
                        cJSON *user_id = cJSON_GetObjectItem(author, "id");
                        
                        if (user_id && cJSON_IsString(user_id) &&
                            channel && cJSON_IsString(channel) &&
                            content && cJSON_IsString(content)) {
                            handle_message(user_id->valuestring, 
                                        channel->valuestring, 
                                        content->valuestring);
                        }
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
}

/**
 * Initialize QQ Channel
 */
mimi_err_t qq_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "QQ Channel already initialized");
        return MIMI_OK;
    }

    /* Check if QQ is enabled */
    const mimi_config_t *config = mimi_config_get();
    if (!config->qq_enabled) {
        MIMI_LOGI(TAG, "QQ Channel is disabled");
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Load credentials from config */
    if (config->qq_app_id[0] != '\0') {
        strncpy(s_priv.app_id, config->qq_app_id, sizeof(s_priv.app_id) - 1);
    }
    if (config->qq_token[0] != '\0') {
        strncpy(s_priv.token, config->qq_token, sizeof(s_priv.token) - 1);
    }

    if (!s_priv.app_id[0] || !s_priv.token[0]) {
        MIMI_LOGW(TAG, "QQ credentials not configured");
    } else {
        MIMI_LOGI(TAG, "QQ initialized with App ID: %.6s***", s_priv.app_id);
    }

    /* Get or create WebSocket Client Gateway */
    s_priv.ws_gateway = gateway_manager_find("ws_client");
    if (!s_priv.ws_gateway) {
        MIMI_LOGE(TAG, "WebSocket Client Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Get or create HTTP Gateway */
    s_priv.http_gateway = gateway_manager_find("http");
    if (!s_priv.http_gateway) {
        MIMI_LOGE(TAG, "HTTP Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Configure WebSocket Client Gateway for QQ */
    /* QQ WebSocket URL: wss://api.sgroup.qq.com/websocket */
    char ws_url[256];
    snprintf(ws_url, sizeof(ws_url), 
             "wss://api.sgroup.qq.com/websocket?compress=0");
    
    mimi_err_t err = ws_client_gateway_configure(ws_url, s_priv.token, 30000, 30000);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to configure WebSocket Client Gateway: %d", err);
        return err;
    }

    /* Configure HTTP Gateway for QQ */
    err = http_gateway_configure("https://api.sgroup.qq.com", 
                               s_priv.token, 30000);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to configure HTTP Gateway: %d", err);
        return err;
    }

    /* Register mapping for Input Processor */
    err = router_register_mapping("qq", "qq");
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to register input processor mapping");
        return err;
    }

    ch->priv_data = &s_priv;
    s_priv.initialized = true;

    MIMI_LOGI(TAG, "QQ Channel initialized");
    return MIMI_OK;
}

/**
 * Start QQ Channel
 */
mimi_err_t qq_channel_start_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        MIMI_LOGE(TAG, "QQ Channel not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        MIMI_LOGW(TAG, "QQ Channel already started");
        return MIMI_OK;
    }

    if (!s_priv.ws_gateway || !s_priv.http_gateway) {
        MIMI_LOGW(TAG, "Cannot start QQ without WebSocket and HTTP Gateways");
        return MIMI_ERR_INVALID_STATE;
    }

    /* Start HTTP Gateway */
    mimi_err_t err = gateway_start(s_priv.http_gateway);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start HTTP Gateway: %d", err);
        return err;
    }

    /* Set WebSocket message handler */
    gateway_set_on_message(s_priv.ws_gateway, on_ws_message, ch);

    /* Start WebSocket Gateway */
    err = gateway_start(s_priv.ws_gateway);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start WebSocket Gateway: %d", err);
        gateway_stop(s_priv.http_gateway);
        return err;
    }

    s_priv.running = true;
    s_priv.started = true;
    MIMI_LOGI(TAG, "QQ Channel started");
    return MIMI_OK;
}

/**
 * Stop QQ Channel
 */
mimi_err_t qq_channel_stop_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return MIMI_OK;
    }

    s_priv.running = false;
    s_priv.started = false;

    /* Stop WebSocket Gateway */
    if (s_priv.ws_gateway) {
        gateway_stop(s_priv.ws_gateway);
    }

    /* Stop HTTP Gateway */
    if (s_priv.http_gateway) {
        gateway_stop(s_priv.http_gateway);
    }

    MIMI_LOGI(TAG, "QQ Channel stopped");
    return MIMI_OK;
}

/**
 * Destroy QQ Channel
 */
void qq_channel_destroy_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return;
    }

    qq_channel_stop_impl(ch);

    /* Unregister mapping */
    router_unregister_mapping("qq");

    memset(&s_priv, 0, sizeof(s_priv));

    MIMI_LOGI(TAG, "QQ Channel destroyed");
}

/**
 * Send message through QQ Channel
 */
mimi_err_t qq_channel_send_impl(channel_t *ch, const char *session_id,
                                       const char *content)
{
    (void)ch;

    if (!s_priv.initialized || !s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    return qq_send_message(session_id, content);
}

/**
 * Check if QQ Channel is running
 */
static bool qq_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.running;
}

/**
 * Set message callback (not used - WebSocket is used)
 */
static void qq_set_on_message(channel_t *ch,
                                void (*cb)(channel_t *, const char *, 
                                           const char *, void *),
                                void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
    /* Messages are handled via WebSocket */
}

/**
 * Set connect callback (not used)
 */
static void qq_set_on_connect(channel_t *ch,
                                void (*cb)(channel_t *, const char *, 
                                           void *),
                                void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Set disconnect callback (not used)
 */
static void qq_set_on_disconnect(channel_t *ch,
                                   void (*cb)(channel_t *, const char *, 
                                              void *),
                                   void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Global QQ Channel instance
 */
channel_t g_qq_channel = {
    .name = "qq",
    .description = "QQ Bot",
    .require_auth = false,
    .max_sessions = 0,
    .init = qq_channel_init_impl,
    .start = qq_channel_start_impl,
    .stop = qq_channel_stop_impl,
    .destroy = qq_channel_destroy_impl,
    .send = qq_channel_send_impl,
    .is_running = qq_is_running_impl,
    .set_on_message = qq_set_on_message,
    .set_on_connect = qq_set_on_connect,
    .set_on_disconnect = qq_set_on_disconnect,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false
};

/**
 * Initialize QQ Channel module
 */
mimi_err_t qq_channel_init(void)
{
    return MIMI_OK;
}
