/**
 * WeChat Channel Implementation
 *
 * Uses HTTP long-polling for WeChat iLink Bot API integration.
 * Similar to Telegram channel - uses polling instead of WebSocket.
 * 
 * Architecture:
 * - wechat_login.c/.h: Manages login state, QR code, token persistence
 * - wechat_channel.c: Message polling, sending, and channel interface
 */

#include "channels/wechat/wechat_channel.h"
#include "channels/wechat/wechat_login.h"
#include "channels/channel_manager.h"
#include "router/router.h"
#include "config.h"
#include "config_view.h"
#include "bus/message_bus.h"
#include "gateway/http/http_client_gateway.h"
#include "gateway/gateway_manager.h"
#include "log.h"
#include "os/os.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>

static const char *TAG = "wechat";
static const int WECHAT_POLL_TIMEOUT_MS = 25000;  /* 25 seconds long-poll */
static const int WECHAT_API_TIMEOUT_MS = 35000;   /* API call timeout */
static const char *WECHAT_BASE_URL = "https://ilinkai.weixin.qq.com/";

/* WeChat Channel private data */
typedef struct {
    bool initialized;
    bool started;
    gateway_t *http_gateway;
    volatile bool running;
    char updates_buf[1024];        /* For incremental message sync */
    int last_message_id;           /* Track last processed message */
} wechat_channel_priv_t;

static wechat_channel_priv_t s_priv = {0};

/* Forward declarations */
static bool wechat_is_running_impl(channel_t *ch);
static void wechat_generate_x_wechat_uin(char *buf, size_t buf_len);

/**
 * Generate random X-WECHAT-UIN header (simplified random string)
 */
static void wechat_generate_x_wechat_uin(char *buf, size_t buf_len)
{
    srand((unsigned int)time(NULL) ^ (unsigned int)mimi_time_ms());
    uint32_t random_uin = (uint32_t)rand();
    
    /* Simple hex encoding instead of base64 */
    snprintf(buf, buf_len, "%08x", random_uin);
}

/**
 * Internal: authenticated HTTP POST with custom timeout
 * (enables POLL_TIMEOUT_MS to be used for long-polling)
 */
static mimi_err_t wechat_http_post_auth_timeout(const char *endpoint, const char *json_body,
                                                char *response, size_t response_len,
                                                int timeout_ms)
{
    if (!s_priv.http_gateway) {
        MIMI_LOGD(TAG, "HTTP gateway not available for request to %s", endpoint);
        return MIMI_ERR_INVALID_STATE;
    }

    const char *token = wechat_login_get_token();
    if (!token) {
        MIMI_LOGD(TAG, "Cannot send authenticated request - not logged in");
        return MIMI_ERR_INVALID_STATE;
    }

    /* Debug: Log token preview and request details */
    MIMI_LOGD(TAG, "Sending authenticated request to: %s", endpoint);
    MIMI_LOGD(TAG, "Using token (preview): %.16s...", token);
    MIMI_LOGD(TAG, "Request timeout: %d ms", timeout_ms);

    /* NOTE: No need to call http_client_gateway_configure() here anymore!
     * We now use _with_token() API which passes token via stack parameter,
     * eliminating race conditions and duplicate initialization warnings */

    /* Prepare WeChat-specific headers (stack allocated - fully reentrant) */
    char x_wechat_uin[64];
    char extra_headers[256];
    wechat_generate_x_wechat_uin(x_wechat_uin, sizeof(x_wechat_uin));
    
    /* WeChat API requires specific headers */
    snprintf(extra_headers, sizeof(extra_headers),
             "AuthorizationType: ilink_bot_token\r\n"
             "X-WECHAT-UIN: %s",
             x_wechat_uin);

    /* Use _with_token() variant: token is passed on caller's stack -> NO RACE CONDITION!
     * This ensures token cannot be modified by concurrent gateway configuration */
    return http_client_gateway_post_with_token(s_priv.http_gateway, endpoint, token, extra_headers,
                                               json_body, strlen(json_body ? json_body : ""),
                                               response, response_len);
}

/**
 * Authenticated HTTP POST to WeChat API (uses default API timeout)
 */
static mimi_err_t wechat_http_post_auth(const char *endpoint, const char *json_body,
                                        char *response, size_t response_len)
{
    return wechat_http_post_auth_timeout(endpoint, json_body, response, response_len,
                                         WECHAT_API_TIMEOUT_MS);
}

/**
 * Handle incoming message from WeChat
 */
static void handle_wechat_message(cJSON *msg)
{
    if (!msg) return;

    cJSON *message_id = cJSON_GetObjectItem(msg, "message_id");
    cJSON *from_user_id = cJSON_GetObjectItem(msg, "from_user_id");
    cJSON *item_list = cJSON_GetObjectItem(msg, "item_list");
    
    if (!from_user_id || !cJSON_IsString(from_user_id) || !item_list) {
        return;
    }

    /* Update last message ID */
    if (message_id && cJSON_IsNumber(message_id)) {
        int msg_id = (int)message_id->valuedouble;
        if (msg_id > s_priv.last_message_id) {
            s_priv.last_message_id = msg_id;
        }
    }

    /* Extract text content from item_list */
    char content[4096] = {0};
    if (cJSON_IsArray(item_list)) {
        cJSON *item;
        cJSON_ArrayForEach(item, item_list) {
            cJSON *type = cJSON_GetObjectItem(item, "type");
            if (type && cJSON_IsNumber(type)) {
                int item_type = (int)type->valuedouble;
                if (item_type == 1) {  /* TEXT */
                    cJSON *text_item = cJSON_GetObjectItem(item, "text_item");
                    if (text_item) {
                        cJSON *text = cJSON_GetObjectItem(text_item, "text");
                        if (text && cJSON_IsString(text)) {
                            strncat(content, text->valuestring, 
                                    sizeof(content) - strlen(content) - 1);
                        }
                    }
                } else if (item_type == 2) {  /* IMAGE */
                    strncat(content, "[图片消息]", sizeof(content) - 1);
                } else if (item_type == 3) {  /* VOICE */
                    cJSON *voice_item = cJSON_GetObjectItem(item, "voice_item");
                    if (voice_item) {
                        cJSON *text = cJSON_GetObjectItem(voice_item, "text");
                        if (text && cJSON_IsString(text)) {
                            strncat(content, "[语音] ", sizeof(content) - 1);
                            strncat(content, text->valuestring, 
                                    sizeof(content) - strlen(content) - 1);
                        } else {
                            strncat(content, "[语音消息]", sizeof(content) - 1);
                        }
                    }
                } else if (item_type == 4) {  /* FILE */
                    strncat(content, "[文件消息]", sizeof(content) - 1);
                } else if (item_type == 5) {  /* VIDEO */
                    strncat(content, "[视频消息]", sizeof(content) - 1);
                }
            }
        }
    }

    if (content[0] == '\0') {
        MIMI_LOGW(TAG, "Empty message content from user %s", from_user_id->valuestring);
        return;
    }

    MIMI_LOGI(TAG, "Incoming message from %s: %.40s...", 
              from_user_id->valuestring, content);

    /* Route through Input Processor - using generic handler
     * Eliminates need for per-channel router boilerplate */
    router_handle_generic("wechat", from_user_id->valuestring, content);
}

/**
 * Main polling task - handles login check and message receiving
 */
static void wechat_poll_task(void *arg)
{
    (void)arg;
    MIMI_LOGI(TAG, "WeChat message polling started");

    while (s_priv.running) {
        /* Check login status first */
        if (!wechat_login_is_logged_in()) {
            wechat_login_state_t state = wechat_login_get_state();
            if (state == WECHAT_LOGIN_STATE_PENDING || 
                state == WECHAT_LOGIN_STATE_SCANNED) {
                /* Poll login status */
                wechat_login_check_status();
                mimi_sleep_ms(2000);
            } else {
                /* Not logged in and no pending login - wait quietly */
                MIMI_LOGD(TAG, "Waiting for WeChat login... (call /wechat_login to start)");
                mimi_sleep_ms(5000);
            }
            continue;
        }

        /* Logged in - poll for messages */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "get_updates_buf", s_priv.updates_buf);
        cJSON *base_info = cJSON_AddObjectToObject(body, "base_info");
        cJSON_AddStringToObject(base_info, "channel_version", "1.0.0");
        
        char *json_body = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        
        if (!json_body) {
            mimi_sleep_ms(1000);
            continue;
        }

        char response[8192];
        response[0] = '\0';
        
        /* Long-poll for messages with extended timeout */
        mimi_err_t err = wechat_http_post_auth_timeout("ilink/bot/getupdates", json_body,
                                                       response, sizeof(response),
                                                       WECHAT_POLL_TIMEOUT_MS);
        free(json_body);
        
        if (err != MIMI_OK) {
            MIMI_LOGW(TAG, "getUpdates failed: %d", err);
            mimi_sleep_ms(2000);
            continue;
        }

        if (response[0] == '\0') {
            MIMI_LOGW(TAG, "getUpdates: empty response");
            mimi_sleep_ms(1000);
            continue;
        }

        /* Debug: Log raw response preview */
        MIMI_LOGD(TAG, "getUpdates response (preview): %.200s", response);
        
        cJSON *root = cJSON_Parse(response);
        if (!root) {
            MIMI_LOGW(TAG, "getUpdates: invalid JSON");
            mimi_sleep_ms(1000);
            continue;
        }

        /* Check for errors - check both 'ret' and 'errcode' */
        cJSON *ret = cJSON_GetObjectItem(root, "ret");
        cJSON *errcode = cJSON_GetObjectItem(root, "errcode");
        
        int error_code = 0;
        const char *error_msg = NULL;
        
        if (ret && cJSON_IsNumber(ret) && ret->valuedouble != 0) {
            error_code = (int)ret->valuedouble;
            cJSON *errmsg = cJSON_GetObjectItem(root, "errmsg");
            error_msg = errmsg && cJSON_IsString(errmsg) ? errmsg->valuestring : "unknown";
        }
        
        if (errcode && cJSON_IsNumber(errcode) && errcode->valuedouble != 0) {
            error_code = (int)errcode->valuedouble;
            cJSON *errmsg = cJSON_GetObjectItem(root, "errmsg");
            error_msg = errmsg && cJSON_IsString(errmsg) ? errmsg->valuestring : "unknown";
        }
        
        if (error_code != 0) {
            MIMI_LOGW(TAG, "WeChat API error: %d - %s", error_code, error_msg ? error_msg : "unknown");
            MIMI_LOGW(TAG, "Full response was: %s", response);
            
            /* Session timeout or invalid token */
            if (error_code == -14) {
                MIMI_LOGW(TAG, "Session-related error (code -14)");
                /* Don't auto-logout for debugging - let's investigate first */
                /* wechat_login_logout(); */
            }
            
            cJSON_Delete(root);
            mimi_sleep_ms(5000);
            continue;
        }

        /* Save updates buffer for next request */
        cJSON *get_updates_buf = cJSON_GetObjectItem(root, "get_updates_buf");
        if (get_updates_buf && cJSON_IsString(get_updates_buf)) {
            strncpy(s_priv.updates_buf, get_updates_buf->valuestring, 
                    sizeof(s_priv.updates_buf) - 1);
        }

        /* Process messages */
        cJSON *msgs = cJSON_GetObjectItem(root, "msgs");
        if (msgs && cJSON_IsArray(msgs)) {
            cJSON *msg;
            cJSON_ArrayForEach(msg, msgs) {
                cJSON *msg_type = cJSON_GetObjectItem(msg, "message_type");
                /* Only process user messages (type 1 = USER) */
                if (!msg_type || !cJSON_IsNumber(msg_type) || 
                    (int)msg_type->valuedouble != 1) {
                    continue;
                }
                handle_wechat_message(msg);
            }
        }

        cJSON_Delete(root);
    }

    MIMI_LOGI(TAG, "WeChat polling stopped");
}

/**
 * Initialize WeChat Channel
 */
static mimi_err_t wechat_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "WeChat Channel already initialized");
        return MIMI_OK;
    }

    /* Check if WeChat is enabled in config */
    mimi_cfg_obj_t wechat_cfg = mimi_cfg_named("channels", "wechat");
    if (!mimi_cfg_get_bool(wechat_cfg, "enabled", false)) {
        MIMI_LOGD(TAG, "WeChat Channel is disabled in config");
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Initialize login manager */
    mimi_err_t err = wechat_login_manager_init();
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to initialize login manager: %d", err);
        return err;
    }

    /* Load credentials from config (auto-login if possible) */
    wechat_login_load_from_config();

    /* Get or create HTTP Gateway */
    s_priv.http_gateway = gateway_manager_find("http");
    if (!s_priv.http_gateway) {
        MIMI_LOGE(TAG, "HTTP Gateway not found");
        wechat_login_manager_cleanup();
        return MIMI_ERR_NOT_FOUND;
    }

    /* Configure HTTP Gateway for WeChat - ONLY to set base_url (NO token here!)
     * Token is now passed per-request via _with_token() API to avoid race conditions */
    err = http_client_gateway_configure(WECHAT_BASE_URL, "", WECHAT_API_TIMEOUT_MS);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to configure HTTP Gateway: %d", err);
        wechat_login_manager_cleanup();
        return err;
    }

    /* Register mapping for Input Processor */
    err = router_register_mapping("wechat", "wechat");
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to register input processor mapping");
        wechat_login_manager_cleanup();
        return err;
    }

    ch->priv_data = &s_priv;
    s_priv.initialized = true;

    if (wechat_login_is_logged_in()) {
        MIMI_LOGI(TAG, "WeChat Channel initialized (already logged in)");
    } else {
        MIMI_LOGI(TAG, "WeChat Channel initialized (not logged in)");
    }
    return MIMI_OK;
}

/**
 * Start WeChat Channel
 */
static mimi_err_t wechat_channel_start_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        MIMI_LOGE(TAG, "WeChat Channel not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        MIMI_LOGW(TAG, "WeChat Channel already started");
        return MIMI_OK;
    }

    if (!s_priv.http_gateway) {
        MIMI_LOGW(TAG, "Cannot start WeChat without HTTP Gateway");
        return MIMI_ERR_INVALID_STATE;
    }

    /* Start HTTP Gateway */
    mimi_err_t err = gateway_start(s_priv.http_gateway);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start HTTP Gateway: %d", err);
        return err;
    }

    /* Start polling task */
    s_priv.running = true;
    err = mimi_task_create_detached("wechat_poll", wechat_poll_task, NULL);
    if (err != MIMI_OK) {
        s_priv.running = false;
        MIMI_LOGE(TAG, "Failed to create wechat poll task: %d", err);
        gateway_stop(s_priv.http_gateway);
        return err;
    }

    s_priv.started = true;
    MIMI_LOGI(TAG, "WeChat Channel started");
    MIMI_LOGI(TAG, "Use '/wechat_login' command to start QR login");

    return MIMI_OK;
}

/**
 * Stop WeChat Channel
 */
static mimi_err_t wechat_channel_stop_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return MIMI_OK;
    }

    s_priv.running = false;
    s_priv.started = false;

    /* Stop HTTP Gateway */
    if (s_priv.http_gateway) {
        gateway_stop(s_priv.http_gateway);
    }

    MIMI_LOGI(TAG, "WeChat Channel stopped");
    return MIMI_OK;
}

/**
 * Destroy WeChat Channel
 */
static void wechat_channel_destroy_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return;
    }

    wechat_channel_stop_impl(ch);

    /* Unregister mapping */
    router_unregister_mapping("wechat");

    /* Cleanup login manager */
    wechat_login_manager_cleanup();

    memset(&s_priv, 0, sizeof(s_priv));

    MIMI_LOGI(TAG, "WeChat Channel destroyed");
}

/**
 * Check if WeChat Channel is running
 */
static bool wechat_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.running;
}

/**
 * Set message callback (messages are handled via polling internally)
 */
static void wechat_set_on_message(channel_t *ch,
                                  void (*cb)(channel_t *, const char *,
                                             const char *, void *),
                                  void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Set connect callback (not used)
 */
static void wechat_set_on_connect(channel_t *ch,
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
static void wechat_set_on_disconnect(channel_t *ch,
                                     void (*cb)(channel_t *, const char *,
                                                void *),
                                     void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Send message to WeChat user
 */
static mimi_err_t wechat_channel_send_msg_impl(channel_t *ch, const mimi_msg_t *msg)
{
    (void)ch;

    if (!s_priv.initialized || !s_priv.started) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!msg || !msg->chat_id[0]) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!wechat_login_is_logged_in()) {
        MIMI_LOGE(TAG, "Not logged in to WeChat - please login first");
        return MIMI_ERR_INVALID_STATE;
    }

    const char *content = msg->content ? msg->content : "";

    /* Build WeChat message structure */
    cJSON *root = cJSON_CreateObject();
    cJSON *msg_obj = cJSON_AddObjectToObject(root, "msg");
    
    cJSON_AddStringToObject(msg_obj, "to_user_id", msg->chat_id);
    
    char client_id[128];
    snprintf(client_id, sizeof(client_id), "mcp-wechat:%lld-%x",
             (long long)time(NULL), rand());
    cJSON_AddStringToObject(msg_obj, "client_id", client_id);
    
    cJSON_AddNumberToObject(msg_obj, "message_type", 2);  /* BOT */
    cJSON_AddNumberToObject(msg_obj, "message_state", 2); /* FINISH */
    
    cJSON *item_list = cJSON_AddArrayToObject(msg_obj, "item_list");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "type", 1);  /* TEXT */
    
    cJSON *text_item = cJSON_AddObjectToObject(item, "text_item");
    cJSON_AddStringToObject(text_item, "text", content);
    
    cJSON_AddItemToArray(item_list, item);
    
    cJSON *base_info = cJSON_AddObjectToObject(root, "base_info");
    cJSON_AddStringToObject(base_info, "channel_version", "1.0.0");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json) {
        return MIMI_ERR_NO_MEM;
    }

    char response[8192];
    response[0] = '\0';
    mimi_err_t err = wechat_http_post_auth("ilink/bot/sendmessage", json,
                                           response, sizeof(response));
    free(json);
    
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to send message: %d", err);
        return err;
    }

    /* Check response for errors */
    if (response[0] != '\0') {
        cJSON *resp = cJSON_Parse(response);
        if (resp) {
            cJSON *errcode = cJSON_GetObjectItem(resp, "errcode");
            if (errcode && cJSON_IsNumber(errcode) && errcode->valuedouble != 0) {
                cJSON *errmsg = cJSON_GetObjectItem(resp, "errmsg");
                MIMI_LOGE(TAG, "WeChat send error: %.0f - %s",
                          errcode->valuedouble,
                          errmsg && cJSON_IsString(errmsg) ? errmsg->valuestring : "unknown");
                cJSON_Delete(resp);
                return MIMI_ERR_FAIL;
            }
            cJSON_Delete(resp);
        }
    }

    return MIMI_OK;
}

/* --- Channel wrapper functions for login management --- */

mimi_err_t wechat_channel_start_qr_login(char *qr_url, size_t qr_url_len)
{
    if (!s_priv.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    mimi_err_t err = wechat_login_start_qr();
    if (err != MIMI_OK) {
        return err;
    }

    const wechat_login_status_t *status = wechat_login_get_status();
    if (qr_url && qr_url_len > 0 && status->qrcode_url[0]) {
        strncpy(qr_url, status->qrcode_url, qr_url_len - 1);
        qr_url[qr_url_len - 1] = '\0';
    }

    return MIMI_OK;
}

mimi_err_t wechat_channel_check_qr_status(char *status_buffer, size_t buffer_len)
{
    if (!s_priv.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    mimi_err_t err = wechat_login_check_status();
    
    if (status_buffer && buffer_len > 0) {
        const wechat_login_status_t *status = wechat_login_get_status();
        snprintf(status_buffer, buffer_len,
                 "{\"state\":\"%d\",\"logged_in\":%s}",
                 status->state,
                 wechat_login_is_logged_in() ? "true" : "false");
    }

    return err;
}

mimi_err_t wechat_channel_set_token(const char *token)
{
    return wechat_login_with_token(token, NULL, NULL);
}

/* --- Standard channel interface functions --- */

void wechat_channel_set_on_message(channel_t *ch,
                                   void (*cb)(channel_t *, const char *,
                                              const char *, void *),
                                   void *user_data)
{
    wechat_set_on_message(ch, cb, user_data);
}

void wechat_channel_set_on_connect(channel_t *ch,
                                   void (*cb)(channel_t *, const char *,
                                              void *),
                                   void *user_data)
{
    wechat_set_on_connect(ch, cb, user_data);
}

void wechat_channel_set_on_disconnect(channel_t *ch,
                                      void (*cb)(channel_t *, const char *,
                                                 void *),
                                      void *user_data)
{
    wechat_set_on_disconnect(ch, cb, user_data);
}

mimi_err_t wechat_channel_init(void)
{
    /* Only initialize login manager here, registration is handled by channel_manager */
    return wechat_login_manager_init();
}

/**
 * Global WeChat Channel instance
 */
channel_t g_wechat_channel = {
    .name = "wechat",
    .description = "WeChat iLink Bot",
    .require_auth = true,
    .max_sessions = -1,
    .init = wechat_channel_init_impl,
    .start = wechat_channel_start_impl,
    .stop = wechat_channel_stop_impl,
    .destroy = wechat_channel_destroy_impl,
    .send_msg = wechat_channel_send_msg_impl,
    .is_running = wechat_is_running_impl,
    .set_on_message = wechat_set_on_message,
    .set_on_connect = wechat_set_on_connect,
    .set_on_disconnect = wechat_set_on_disconnect,
    .send_control = NULL,
    .set_on_control_response = NULL,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false,
};
