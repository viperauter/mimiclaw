/**
 * WeChat Login Manager Implementation
 */

#include "channels/wechat/wechat_login.h"
#include "gateway/http/http_client_gateway.h"
#include "gateway/gateway_manager.h"
#include "config.h"
#include "config_view.h"
#include "log.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "wechat_login";
static const int QR_EXPIRY_SECONDS = 300;  /* 5 minutes */
static const char *WECHAT_BASE_URL = "https://ilinkai.weixin.qq.com/";

static wechat_login_status_t s_status = {0};
static wechat_login_state_cb s_callback = NULL;
static void *s_callback_user_data = NULL;
static bool s_initialized = false;

/* Forward declarations */
static void notify_state_change(wechat_login_state_t new_state);
static gateway_t* get_http_gateway(void);

mimi_err_t wechat_login_manager_init(void)
{
    if (s_initialized) {
        return MIMI_OK;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = WECHAT_LOGIN_STATE_IDLE;
    s_callback = NULL;
    s_callback_user_data = NULL;
    s_initialized = true;

    /* Try to load credentials from config first */
    wechat_login_load_from_config();

    MIMI_LOGI(TAG, "Login manager initialized");
    return MIMI_OK;
}

void wechat_login_manager_cleanup(void)
{
    if (!s_initialized) {
        return;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = WECHAT_LOGIN_STATE_IDLE;
    s_callback = NULL;
    s_callback_user_data = NULL;
    s_initialized = false;

    MIMI_LOGI(TAG, "Login manager cleaned up");
}

static gateway_t* get_http_gateway(void)
{
    gateway_t *gw = gateway_manager_find("http");
    if (!gw) {
        MIMI_LOGE(TAG, "HTTP Gateway not found");
        return NULL;
    }
    return gw;
}

static void notify_state_change(wechat_login_state_t new_state)
{
    if (s_status.state != new_state) {
        s_status.state = new_state;
        
        if (s_callback) {
            s_callback(new_state, &s_status, s_callback_user_data);
        }
    }
}

mimi_err_t wechat_login_start_qr(void)
{
    if (!s_initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    gateway_t *gw = gateway_manager_find("http");
    if (!gw) {
        MIMI_LOGE(TAG, "HTTP Gateway not found");
        return MIMI_ERR_NOT_FOUND;
    }

    /* Clear pending state */
    s_status.qrcode_url[0] = '\0';
    s_status.qrcode_created_at = 0;
    s_status.last_error[0] = '\0';

    char response[4096];
    response[0] = '\0';
    
    /* QR login API: use WeChat base_url, no auth token needed */
    mimi_err_t err = http_client_gateway_get(gw,
                                             "ilink/bot/get_bot_qrcode?bot_type=3",
                                             HTTP_OPTS_BASE(WECHAT_BASE_URL),
                                             response, sizeof(response));
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to get QR code: %d", err);
        snprintf(s_status.last_error, sizeof(s_status.last_error),
                 "HTTP error: %d", err);
        notify_state_change(WECHAT_LOGIN_STATE_ERROR);
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        MIMI_LOGE(TAG, "Invalid QR code response");
        snprintf(s_status.last_error, sizeof(s_status.last_error),
                 "Invalid JSON response");
        notify_state_change(WECHAT_LOGIN_STATE_ERROR);
        return MIMI_ERR_INVALID_ARG;
    }

    cJSON *qrcode = cJSON_GetObjectItem(root, "qrcode");
    cJSON *qrcode_img_content = cJSON_GetObjectItem(root, "qrcode_img_content");
    
    if (!qrcode || !cJSON_IsString(qrcode) ||
        !qrcode_img_content || !cJSON_IsString(qrcode_img_content)) {
        cJSON_Delete(root);
        MIMI_LOGE(TAG, "QR code response missing required fields");
        snprintf(s_status.last_error, sizeof(s_status.last_error),
                 "Missing required fields");
        notify_state_change(WECHAT_LOGIN_STATE_ERROR);
        return MIMI_ERR_INVALID_ARG;
    }

    /* Store QR info */
    strncpy(s_status.qrcode_id, qrcode->valuestring,
            sizeof(s_status.qrcode_id) - 1);
    s_status.qrcode_id[sizeof(s_status.qrcode_id) - 1] = '\0';
    
    strncpy(s_status.qrcode_url, qrcode_img_content->valuestring,
            sizeof(s_status.qrcode_url) - 1);
    s_status.qrcode_url[sizeof(s_status.qrcode_url) - 1] = '\0';
    
    s_status.qrcode_created_at = time(NULL);

    cJSON_Delete(root);
    
    notify_state_change(WECHAT_LOGIN_STATE_PENDING);
    MIMI_LOGI(TAG, "QR code generated successfully");
    MIMI_LOGI(TAG, "QR ID: %s", s_status.qrcode_id);
    MIMI_LOGI(TAG, "QR URL: %s", s_status.qrcode_url);
    return MIMI_OK;
}

mimi_err_t wechat_login_check_status(void)
{
    if (!s_initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_status.state != WECHAT_LOGIN_STATE_PENDING &&
        s_status.state != WECHAT_LOGIN_STATE_SCANNED) {
        return MIMI_ERR_INVALID_STATE;
    }

    /* Check if QR code is expired */
    if (time(NULL) - s_status.qrcode_created_at > QR_EXPIRY_SECONDS) {
        notify_state_change(WECHAT_LOGIN_STATE_EXPIRED);
        MIMI_LOGW(TAG, "QR code expired");
        return MIMI_ERR_TIMEOUT;
    }

    gateway_t *gw = get_http_gateway();
    if (!gw) {
        return MIMI_ERR_NOT_FOUND;
    }

    /* Build endpoint with QR code parameter */
    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint),
             "ilink/bot/get_qrcode_status?qrcode=%s",
             s_status.qrcode_id);

    /* QR status API: needs iLink-App-ClientVersion header */
    http_request_options_t opts = {
        .base_url = WECHAT_BASE_URL,
        .auth_token = NULL,
        .extra_headers = "iLink-App-ClientVersion: 1\r\n",
        .timeout_ms = 0
    };

    char response[4096];
    response[0] = '\0';

    mimi_err_t err = http_client_gateway_get(gw, endpoint, &opts,
                                             response, sizeof(response));
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to check QR status: %d", err);
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        MIMI_LOGE(TAG, "Invalid QR status response");
        return MIMI_ERR_INVALID_ARG;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status && cJSON_IsString(status)) {
        if (strcmp(status->valuestring, "wait") == 0) {
            /* Still waiting */
        } else if (strcmp(status->valuestring, "scaned") == 0) {
            notify_state_change(WECHAT_LOGIN_STATE_SCANNED);
        } else if (strcmp(status->valuestring, "confirmed") == 0) {
            /* Login successful */
            cJSON *bot_token = cJSON_GetObjectItem(root, "bot_token");
            cJSON *ilink_bot_id = cJSON_GetObjectItem(root, "ilink_bot_id");
            cJSON *ilink_user_id = cJSON_GetObjectItem(root, "ilink_user_id");
            
            if (bot_token && cJSON_IsString(bot_token)) {
                strncpy(s_status.bot_token, bot_token->valuestring,
                        sizeof(s_status.bot_token) - 1);
            }
            
            if (ilink_bot_id && cJSON_IsString(ilink_bot_id)) {
                strncpy(s_status.bot_id, ilink_bot_id->valuestring,
                        sizeof(s_status.bot_id) - 1);
            }
            
            if (ilink_user_id && cJSON_IsString(ilink_user_id)) {
                strncpy(s_status.user_id, ilink_user_id->valuestring,
                        sizeof(s_status.user_id) - 1);
            }

            notify_state_change(WECHAT_LOGIN_STATE_LOGGED_IN);
            
            /* Auto-save to config */
            wechat_login_save_to_config();
            
            MIMI_LOGI(TAG, "WeChat login successful!");
        } else if (strcmp(status->valuestring, "expired") == 0) {
            notify_state_change(WECHAT_LOGIN_STATE_EXPIRED);
        }
    }

    cJSON_Delete(root);
    return MIMI_OK;
}

wechat_login_state_t wechat_login_get_state(void)
{
    return s_status.state;
}

const wechat_login_status_t* wechat_login_get_status(void)
{
    return &s_status;
}

bool wechat_login_is_logged_in(void)
{
    return (s_status.state == WECHAT_LOGIN_STATE_LOGGED_IN) &&
           (s_status.bot_token[0] != '\0');
}

const char* wechat_login_get_token(void)
{
    if (wechat_login_is_logged_in()) {
        return s_status.bot_token;
    }
    return NULL;
}

const char* wechat_login_get_bot_id(void)
{
    if (wechat_login_is_logged_in()) {
        return s_status.bot_id;
    }
    return NULL;
}

const char* wechat_login_get_user_id(void)
{
    if (wechat_login_is_logged_in()) {
        return s_status.user_id;
    }
    return NULL;
}

mimi_err_t wechat_login_with_token(const char *bot_token, 
                                   const char *bot_id,
                                   const char *user_id)
{
    if (!s_initialized || !bot_token || !bot_token[0]) {
        return MIMI_ERR_INVALID_ARG;
    }

    strncpy(s_status.bot_token, bot_token, sizeof(s_status.bot_token) - 1);
    s_status.bot_token[sizeof(s_status.bot_token) - 1] = '\0';

    if (bot_id) {
        strncpy(s_status.bot_id, bot_id, sizeof(s_status.bot_id) - 1);
        s_status.bot_id[sizeof(s_status.bot_id) - 1] = '\0';
    }

    if (user_id) {
        strncpy(s_status.user_id, user_id, sizeof(s_status.user_id) - 1);
        s_status.user_id[sizeof(s_status.user_id) - 1] = '\0';
    }

    notify_state_change(WECHAT_LOGIN_STATE_LOGGED_IN);
    MIMI_LOGI(TAG, "Logged in with provided token");
    return MIMI_OK;
}

void wechat_login_logout(void)
{
    if (!s_initialized) {
        return;
    }

    memset(s_status.bot_token, 0, sizeof(s_status.bot_token));
    memset(s_status.bot_id, 0, sizeof(s_status.bot_id));
    memset(s_status.user_id, 0, sizeof(s_status.user_id));
    s_status.qrcode_url[0] = '\0';
    s_status.qrcode_created_at = 0;
    s_status.last_error[0] = '\0';

    notify_state_change(WECHAT_LOGIN_STATE_IDLE);
    MIMI_LOGI(TAG, "Logged out");
}

void wechat_login_set_callback(wechat_login_state_cb cb, void *user_data)
{
    s_callback = cb;
    s_callback_user_data = user_data;
}

mimi_err_t wechat_login_save_to_config(void)
{
    if (!s_status.bot_token[0]) {
        MIMI_LOGE(TAG, "No credentials to save");
        return MIMI_ERR_INVALID_STATE;
    }
    
    /* Update JSON root via generic config API */
    mimi_err_t err = MIMI_OK;
    
    /* Set credentials using dot-path format */
    if (s_status.bot_token[0]) {
        err = mimi_config_set_string("channels.wechat.bot_token", s_status.bot_token);
        if (err != MIMI_OK) goto cleanup;
    }
    if (s_status.bot_id[0]) {
        err = mimi_config_set_string("channels.wechat.bot_id", s_status.bot_id);
        if (err != MIMI_OK) goto cleanup;
    }
    if (s_status.user_id[0]) {
        err = mimi_config_set_string("channels.wechat.user_id", s_status.user_id);
        if (err != MIMI_OK) goto cleanup;
    }
    
    /* Enable WeChat channel when credentials are set */
    if (s_status.bot_token[0]) {
        err = mimi_config_set_bool("channels.wechat.enabled", true);
        if (err != MIMI_OK) goto cleanup;
    }
    
    /* Persist to disk - uses remembered path from mimi_config_load() */
    err = mimi_config_save_current();
    if (err != MIMI_OK) {
        const char *path = mimi_config_get_path();
        if (path) {
            MIMI_LOGE(TAG, "Failed to save config to %s: %d", path, err);
        } else {
            MIMI_LOGE(TAG, "Failed to save config (unknown path): %d", err);
        }
        return err;
    }
    
cleanup:
    if (err == MIMI_OK) {
        MIMI_LOGI(TAG, "Credentials saved to config successfully");
    }
    return err;
}

mimi_err_t wechat_login_load_from_config(void)
{
    mimi_cfg_obj_t wechat_cfg = mimi_cfg_named("channels", "wechat");
    
    bool enabled = mimi_cfg_get_bool(wechat_cfg, "enabled", false);
    if (!enabled) {
        MIMI_LOGD(TAG, "WeChat channel not enabled in config");
        return MIMI_ERR_NOT_SUPPORTED;
    }

    const char *token = mimi_cfg_get_str(wechat_cfg, "bot_token", "");
    const char *bot_id = mimi_cfg_get_str(wechat_cfg, "bot_id", "");
    const char *user_id = mimi_cfg_get_str(wechat_cfg, "user_id", "");

    if (token && token[0] != '\0') {
        strncpy(s_status.bot_token, token, sizeof(s_status.bot_token) - 1);
        s_status.bot_token[sizeof(s_status.bot_token) - 1] = '\0';
        
        if (bot_id && bot_id[0] != '\0') {
            strncpy(s_status.bot_id, bot_id, sizeof(s_status.bot_id) - 1);
            s_status.bot_id[sizeof(s_status.bot_id) - 1] = '\0';
        }
        
        if (user_id && user_id[0] != '\0') {
            strncpy(s_status.user_id, user_id, sizeof(s_status.user_id) - 1);
            s_status.user_id[sizeof(s_status.user_id) - 1] = '\0';
        }

        s_status.state = WECHAT_LOGIN_STATE_LOGGED_IN;
        MIMI_LOGI(TAG, "Loaded credentials from config (bot_id=%s)", 
                  s_status.bot_id[0] ? s_status.bot_id : "unknown");
        return MIMI_OK;
    }

    return MIMI_ERR_NOT_FOUND;
}
