/**
 * Feishu (Lark) Channel Implementation
 *
 * Uses WebSocket Client Gateway for Feishu Bot integration.
 * Provides real-time message handling via WebSocket connection.
 */

#include "channels/feishu/feishu_channel.h"
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

static const char *TAG = "feishu";

/* Feishu Channel private data */
typedef struct {
    bool initialized;
    bool started;
    gateway_t *ws_gateway;
    gateway_t *http_gateway;
    char app_id[64];
    char app_secret[128];
    char tenant_access_token[512];
    volatile bool running;
} feishu_channel_priv_t;

static feishu_channel_priv_t s_priv = {0};

/* Forward declarations */
static bool feishu_is_running_impl(channel_t *ch);
static mimi_err_t feishu_get_ws_url(char *ws_url, size_t ws_url_size);

/**
 * Get tenant access token from Feishu
 */
static mimi_err_t feishu_get_tenant_token(void)
{
    if (!s_priv.app_id[0] || !s_priv.app_secret[0]) {
        return MIMI_ERR_INVALID_STATE;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "app_id", s_priv.app_id);
    cJSON_AddStringToObject(body, "app_secret", s_priv.app_secret);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return MIMI_ERR_NO_MEM;

    char response[4096];
    mimi_err_t err = http_gateway_post(s_priv.http_gateway, 
                                          "/open-apis/auth/v3/tenant_access_token/internal",
                                          json, strlen(json),
                                          response, sizeof(response));
    free(json);
    
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to get tenant token: %d", err);
        return err;
    }

    if (response[0] == '\0') {
        MIMI_LOGE(TAG, "Empty tenant token response");
        return MIMI_ERR_FAIL;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        MIMI_LOGE(TAG, "Invalid tenant token response");
        return MIMI_ERR_FAIL;
    }

    cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
    if (token && cJSON_IsString(token)) {
        strncpy(s_priv.tenant_access_token, token->valuestring,
                sizeof(s_priv.tenant_access_token) - 1);
        MIMI_LOGI(TAG, "Tenant token acquired");
        err = MIMI_OK;
    } else {
        MIMI_LOGE(TAG, "Failed to get tenant token");
        err = MIMI_ERR_FAIL;
    }

    cJSON_Delete(root);
    return err;
}

/**
 * Get Feishu WebSocket connection URL via /callback/ws/endpoint
 * This follows the official Feishu SDK flow using AppID/AppSecret.
 */
static mimi_err_t feishu_get_ws_url(char *ws_url, size_t ws_url_size)
{
    if (!ws_url || ws_url_size == 0) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!s_priv.app_id[0] || !s_priv.app_secret[0]) {
        MIMI_LOGE(TAG, "Feishu AppID/AppSecret not configured");
        return MIMI_ERR_INVALID_STATE;
    }

    if (!s_priv.http_gateway) {
        MIMI_LOGE(TAG, "HTTP Gateway not initialized for Feishu");
        return MIMI_ERR_INVALID_STATE;
    }

    /* Build request body: {"AppID": "...", "AppSecret": "..."} */
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return MIMI_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "AppID", s_priv.app_id);
    cJSON_AddStringToObject(body, "AppSecret", s_priv.app_secret);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        return MIMI_ERR_NO_MEM;
    }

    char response[4096];
    mimi_err_t err = http_gateway_post(s_priv.http_gateway,
                                       "/callback/ws/endpoint",
                                       json, strlen(json),
                                       response, sizeof(response));
    free(json);

    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to get Feishu WS endpoint: %d", err);
        return err;
    }

    if (response[0] == '\0') {
        MIMI_LOGE(TAG, "Empty Feishu WS endpoint response");
        return MIMI_ERR_FAIL;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        MIMI_LOGE(TAG, "Invalid Feishu WS endpoint response: %s", response);
        return MIMI_ERR_FAIL;
    }

    /* According to Feishu SDK, URL field is in data.URL */
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *url = data ? cJSON_GetObjectItem(data, "URL") : NULL;
    if (url && cJSON_IsString(url)) {
        strncpy(ws_url, url->valuestring, ws_url_size - 1);
        ws_url[ws_url_size - 1] = '\0';
        MIMI_LOGI(TAG, "Feishu WS URL acquired: %.64s...", ws_url);
        cJSON_Delete(root);
        return MIMI_OK;
    }

    /* Log full response for debugging if URL not found */
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *msg = cJSON_GetObjectItem(root, "msg");
    if (code && cJSON_IsNumber(code)) {
        MIMI_LOGE(TAG, "Feishu endpoints returned error, code=%d, msg=%s, resp=%s",
                  code->valueint,
                  (msg && cJSON_IsString(msg)) ? msg->valuestring : "(null)",
                  response);
    } else {
        MIMI_LOGE(TAG, "Failed to parse Feishu WS URL from endpoints response: %s", response);
    }
    cJSON_Delete(root);
    return MIMI_ERR_FAIL;
}

/**
 * Send message to Feishu user
 */
static mimi_err_t feishu_send_message(const char *user_id, const char *content)
{
    if (!s_priv.tenant_access_token[0]) {
        mimi_err_t err = feishu_get_tenant_token();
        if (err != MIMI_OK) {
            return err;
        }
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "receive_id", user_id);
    cJSON_AddStringToObject(body, "msg_type", "text");

    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "text", content);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);

    cJSON_AddStringToObject(body, "content", content_str);
    free(content_str);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return MIMI_ERR_NO_MEM;

    char response[4096];
    mimi_err_t err = http_gateway_post(s_priv.http_gateway,
                                          "/im/v1/messages?receive_id_type=open_id",
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
 * Handle incoming message from Feishu
 */
static void handle_message(const char *user_id, const char *content)
{
    MIMI_LOGI(TAG, "Incoming message from %s: %.40s...", user_id, content);

    /* Route through Input Processor */
    router_handle_feishu(user_id, content);
}

/**
 * WebSocket message handler for Feishu
 */
static void on_ws_message(gateway_t *gw, const char *session_id,
                         const char *content, void *user_data)
{
    (void)session_id;
    (void)user_data;
    
    if (!content) {
        return;
    }

    MIMI_LOGI(TAG, "Received WebSocket message: %s", content);

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        MIMI_LOGW(TAG, "Invalid WebSocket message format");
        return;
    }

    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *event = cJSON_GetObjectItem(root, "event");

    if (header && event) {
        cJSON *event_type = cJSON_GetObjectItem(header, "event_type");
        if (event_type && cJSON_IsString(event_type) && 
            strcmp(event_type->valuestring, "im.message.receive_v1") == 0) {
            
            cJSON *sender = cJSON_GetObjectItem(event, "sender");
            cJSON *message = cJSON_GetObjectItem(event, "message");
            
            if (sender && message) {
                cJSON *sender_id = cJSON_GetObjectItem(sender, "sender_id");
                cJSON *content = cJSON_GetObjectItem(message, "content");
                
                if (sender_id && content && cJSON_IsString(content)) {
                    cJSON *content_obj = cJSON_Parse(content->valuestring);
                    if (content_obj) {
                        cJSON *text = cJSON_GetObjectItem(content_obj, "text");
                        if (text && cJSON_IsString(text)) {
                            handle_message(sender_id->valuestring, text->valuestring);
                        }
                        cJSON_Delete(content_obj);
                    } else {
                        MIMI_LOGW(TAG, "Failed to parse content JSON: %s", content->valuestring);
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
}

/**
 * Initialize Feishu Channel
 */
mimi_err_t feishu_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "Feishu Channel already initialized");
        return MIMI_OK;
    }

    /* Check if Feishu is enabled */
    const mimi_config_t *config = mimi_config_get();
    MIMI_LOGI(TAG, "Feishu config: enabled=%d, app_id=%s", config->feishu_enabled, config->feishu_app_id);
    if (!config->feishu_enabled) {
        MIMI_LOGI(TAG, "Feishu Channel is disabled");
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Load credentials from config */
    if (config->feishu_app_id[0] != '\0') {
        strncpy(s_priv.app_id, config->feishu_app_id, sizeof(s_priv.app_id) - 1);
    }
    if (config->feishu_app_secret[0] != '\0') {
        strncpy(s_priv.app_secret, config->feishu_app_secret, sizeof(s_priv.app_secret) - 1);
    }

    if (!s_priv.app_id[0] || !s_priv.app_secret[0]) {
        MIMI_LOGW(TAG, "Feishu credentials not configured");
    } else {
        MIMI_LOGI(TAG, "Feishu initialized with App ID: %.6s***", s_priv.app_id);
    }

    /* Get or create WebSocket Client Gateway */
    s_priv.ws_gateway = gateway_manager_find("ws_client");
    if (!s_priv.ws_gateway) {
        MIMI_LOGE(TAG, "WebSocket Client Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Set WebSocket message handler early to avoid missing initial messages */
    gateway_set_on_message(s_priv.ws_gateway, on_ws_message, ch);

    /* Get or create HTTP Gateway */
    s_priv.http_gateway = gateway_manager_find("http");
    if (!s_priv.http_gateway) {
        MIMI_LOGE(TAG, "HTTP Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Configure HTTP Gateway for Feishu (base domain only) */
    mimi_err_t err = http_gateway_configure("https://open.feishu.cn",
                                            NULL, 30000);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to configure HTTP Gateway: %d", err);
        return err;
    }

    /* Register mapping for Input Processor */
    err = router_register_mapping("feishu", "feishu");
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to register input processor mapping");
        return err;
    }

    ch->priv_data = &s_priv;
    s_priv.initialized = true;

    MIMI_LOGI(TAG, "Feishu Channel initialized");
    return MIMI_OK;
}

/**
 * Start Feishu Channel
 */
mimi_err_t feishu_channel_start_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        MIMI_LOGE(TAG, "Feishu Channel not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        MIMI_LOGW(TAG, "Feishu Channel already started");
        return MIMI_OK;
    }

    if (!s_priv.ws_gateway || !s_priv.http_gateway) {
        MIMI_LOGW(TAG, "Cannot start Feishu without WebSocket and HTTP Gateways");
        return MIMI_ERR_INVALID_STATE;
    }

    /* Start HTTP Gateway */
    mimi_err_t err = gateway_start(s_priv.http_gateway);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start HTTP Gateway: %d", err);
        return err;
    }

    /* Prepare tenant access token for legacy hyper-event fallback */
    if (s_priv.tenant_access_token[0] == '\0') {
        err = feishu_get_tenant_token();
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to get tenant access token: %d", err);
        }
    }

    /*
     * Preferred: use Feishu official /open-apis/bot/v3/endpoints to get dynamic
     * WebSocket URL. If this fails (e.g. 404 or feature not enabled), fall
     * back to legacy hyper-event URL with tenant_access_token auth.
     */
    char ws_url[256];
    const char *ws_token = NULL;
    err = feishu_get_ws_url(ws_url, sizeof(ws_url));
    if (err == MIMI_OK) {
        ws_token = NULL; /* auth embedded in URL */
    } else {
        MIMI_LOGW(TAG, "Feishu endpoints not available, falling back to hyper-event URL");
        snprintf(ws_url, sizeof(ws_url),
                 "wss://open.feishu.cn/open-apis/bot/v3/hyper-event?app_id=%s",
                 s_priv.app_id);
        if (s_priv.tenant_access_token[0] != '\0') {
            ws_token = s_priv.tenant_access_token;
        } else {
            ws_token = NULL;
        }
    }

    /* Configure WebSocket Client Gateway */
    err = ws_client_gateway_configure(ws_url, ws_token, 30000, 30000);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to configure WebSocket Client Gateway: %d", err);
        gateway_stop(s_priv.http_gateway);
        return err;
    }

    /* Start WebSocket Gateway */
    err = gateway_start(s_priv.ws_gateway);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start WebSocket Gateway: %d", err);
        gateway_stop(s_priv.http_gateway);
        return err;
    }

    s_priv.running = true;
    s_priv.started = true;
    MIMI_LOGI(TAG, "Feishu Channel started");
    return MIMI_OK;
}

/**
 * Stop Feishu Channel
 */
mimi_err_t feishu_channel_stop_impl(channel_t *ch)
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

    MIMI_LOGI(TAG, "Feishu Channel stopped");
    return MIMI_OK;
}

/**
 * Destroy Feishu Channel
 */
void feishu_channel_destroy_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return;
    }

    feishu_channel_stop_impl(ch);

    /* Unregister mapping */
    router_unregister_mapping("feishu");

    memset(&s_priv, 0, sizeof(s_priv));

    MIMI_LOGI(TAG, "Feishu Channel destroyed");
}

/**
 * Send message through Feishu Channel
 */
mimi_err_t feishu_channel_send_impl(channel_t *ch, const char *session_id,
                                       const char *content)
{
    (void)ch;

    if (!s_priv.initialized || !s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    return feishu_send_message(session_id, content);
}

/**
 * Check if Feishu Channel is running
 */
static bool feishu_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.running;
}

/**
 * Set message callback (not used - WebSocket is used)
 */
static void feishu_set_on_message(channel_t *ch,
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
static void feishu_set_on_connect(channel_t *ch,
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
static void feishu_set_on_disconnect(channel_t *ch,
                                   void (*cb)(channel_t *, const char *, 
                                              void *),
                                   void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Global Feishu Channel instance
 */
channel_t g_feishu_channel = {
    .name = "feishu",
    .description = "Feishu (Lark) Bot",
    .require_auth = false,
    .max_sessions = 0,
    .init = feishu_channel_init_impl,
    .start = feishu_channel_start_impl,
    .stop = feishu_channel_stop_impl,
    .destroy = feishu_channel_destroy_impl,
    .send = feishu_channel_send_impl,
    .is_running = feishu_is_running_impl,
    .set_on_message = feishu_set_on_message,
    .set_on_connect = feishu_set_on_connect,
    .set_on_disconnect = feishu_set_on_disconnect,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false
};

/**
 * Initialize Feishu Channel module
 */
mimi_err_t feishu_channel_init(void)
{
    return MIMI_OK;
}
