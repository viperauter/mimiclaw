/**
 * Feishu Channel startup state machine (async).
 */

#include "channels/feishu/feishu_startup.h"
#include "channels/feishu/feishu_priv.h"

#include "gateway/websocket/ws_client_gateway.h"
#include "gateway/http/http_client_gateway.h"
#include "log.h"

#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "feishu";

typedef void (*feishu_http_async_cb)(mimi_err_t err, mimi_http_response_t *resp, void *user_data);

static mimi_err_t feishu_http_post_json_async(const char *path, const char *json_body,
                                              feishu_http_async_cb cb, void *user_data);
static void feishu_sm_fail(mimi_err_t err, const char *why);
static void feishu_sm_on_tenant_token(mimi_err_t err, mimi_http_response_t *resp, void *user_data);
static void feishu_sm_on_ws_url(mimi_err_t err, mimi_http_response_t *resp, void *user_data);

static mimi_err_t feishu_http_post_json_async(const char *path, const char *json_body,
                                              feishu_http_async_cb cb, void *user_data)
{
    if (!path || !path[0] || !cb) return MIMI_ERR_INVALID_ARG;

    char url[512];
    snprintf(url, sizeof(url), "%s%s", "https://open.feishu.cn", path);

    const char *headers = "Content-Type: application/json\r\n";
    mimi_http_request_t req = {
        .method = "POST",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)json_body,
        .body_len = json_body ? strlen(json_body) : 0,
        .timeout_ms = 30000,
    };

    mimi_http_response_t *resp = (mimi_http_response_t *)calloc(1, sizeof(*resp));
    if (!resp) return MIMI_ERR_NO_MEM;

    mimi_err_t err = mimi_http_exec_async(&req, resp, cb, user_data);
    if (err != MIMI_OK) {
        free(resp);
        return err;
    }
    return MIMI_OK;
}

static void feishu_sm_fail(mimi_err_t err, const char *why)
{
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!p || p->stopping) return;
    p->sm_state = FEISHU_SM_FAILED;
    MIMI_LOGE(TAG, "Feishu startup failed (%s): %d", why ? why : "unknown", err);
}

static void feishu_sm_on_tenant_token(mimi_err_t err, mimi_http_response_t *resp, void *user_data)
{
    (void)user_data;
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!p) {
        if (resp) { mimi_http_response_free(resp); free(resp); }
        return;
    }

    if (p->stopping) {
        if (resp) { mimi_http_response_free(resp); free(resp); }
        return;
    }

    if (err != MIMI_OK || !resp || !resp->body || resp->body_len == 0) {
        feishu_sm_fail(err != MIMI_OK ? err : MIMI_ERR_FAIL, "tenant_token");
        /* still allow fallback ws url without token */
        p->tenant_access_token[0] = '\0';
    } else {
        cJSON *root = cJSON_Parse((const char *)resp->body);
        if (!root) {
            feishu_sm_fail(MIMI_ERR_FAIL, "tenant_token_parse");
        } else {
            cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
            if (token && cJSON_IsString(token)) {
                strncpy(p->tenant_access_token, token->valuestring,
                        sizeof(p->tenant_access_token) - 1);
                p->tenant_access_token[sizeof(p->tenant_access_token) - 1] = '\0';
                MIMI_LOGI(TAG, "Tenant token acquired");
            } else {
                feishu_sm_fail(MIMI_ERR_FAIL, "tenant_token_missing");
            }
            cJSON_Delete(root);
        }
    }

    if (resp) { mimi_http_response_free(resp); free(resp); }

    /* Next: request WS URL (official endpoint). */
    p->sm_state = FEISHU_SM_GET_WS_URL;
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        feishu_sm_fail(MIMI_ERR_NO_MEM, "ws_url_body");
        return;
    }
    cJSON_AddStringToObject(body, "AppID", p->app_id);
    cJSON_AddStringToObject(body, "AppSecret", p->app_secret);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        feishu_sm_fail(MIMI_ERR_NO_MEM, "ws_url_json");
        return;
    }

    mimi_err_t e2 = feishu_http_post_json_async("/callback/ws/endpoint", json, feishu_sm_on_ws_url, NULL);
    free(json);
    if (e2 != MIMI_OK) {
        feishu_sm_fail(e2, "ws_url_request");
    }
}

static void feishu_sm_on_ws_url(mimi_err_t err, mimi_http_response_t *resp, void *user_data)
{
    (void)user_data;
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!p) {
        if (resp) { mimi_http_response_free(resp); free(resp); }
        return;
    }

    if (p->stopping) {
        if (resp) { mimi_http_response_free(resp); free(resp); }
        return;
    }

    p->ws_url[0] = '\0';
    p->ws_token = NULL;

    bool ok = (err == MIMI_OK && resp && resp->body && resp->body_len > 0);
    if (ok) {
        cJSON *root = cJSON_Parse((const char *)resp->body);
        if (root) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            cJSON *url = data ? cJSON_GetObjectItem(data, "URL") : NULL;
            if (url && cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
                strncpy(p->ws_url, url->valuestring, sizeof(p->ws_url) - 1);
                p->ws_url[sizeof(p->ws_url) - 1] = '\0';
                p->ws_token = NULL; /* auth embedded */
                ok = true;
            } else {
                ok = false;
            }
            cJSON_Delete(root);
        } else {
            ok = false;
        }
    }

    if (!ok) {
        MIMI_LOGW(TAG, "Feishu endpoints not available, falling back to hyper-event URL");
        snprintf(p->ws_url, sizeof(p->ws_url),
                 "wss://open.feishu.cn/open-apis/bot/v3/hyper-event?app_id=%s",
                 p->app_id);
        p->ws_token = (p->tenant_access_token[0] != '\0') ? p->tenant_access_token : NULL;
    }

    if (resp) { mimi_http_response_free(resp); free(resp); }

    /* Configure + start WS gateway (non-blocking). */
    p->sm_state = FEISHU_SM_START_WS;
    mimi_err_t cfg_err = ws_client_gateway_configure(p->ws_url, p->ws_token, 30000, 30000);
    if (cfg_err != MIMI_OK) {
        feishu_sm_fail(cfg_err, "ws_configure");
        return;
    }

    mimi_err_t start_err = gateway_start(p->ws_gateway);
    if (start_err != MIMI_OK) {
        feishu_sm_fail(start_err, "ws_start");
        return;
    }

    p->running = true;
    p->started = true;
    p->sm_state = FEISHU_SM_RUNNING;
    MIMI_LOGI(TAG, "Feishu Channel started (async state machine)");
}

void feishu_sm_start(channel_t *ch)
{
    (void)ch;
    feishu_channel_priv_t *p = feishu_priv_get();
    if (!p) return;

    if (p->stopping) return;
    if (!p->app_id[0] || !p->app_secret[0]) {
        feishu_sm_fail(MIMI_ERR_INVALID_STATE, "credentials");
        return;
    }

    p->sm_state = FEISHU_SM_GET_TENANT_TOKEN;

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        feishu_sm_fail(MIMI_ERR_NO_MEM, "tenant_body");
        return;
    }
    cJSON_AddStringToObject(body, "app_id", p->app_id);
    cJSON_AddStringToObject(body, "app_secret", p->app_secret);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) {
        feishu_sm_fail(MIMI_ERR_NO_MEM, "tenant_json");
        return;
    }

    mimi_err_t e = feishu_http_post_json_async("/open-apis/auth/v3/tenant_access_token/internal",
                                               json, feishu_sm_on_tenant_token, NULL);
    free(json);
    if (e != MIMI_OK) {
        feishu_sm_fail(e, "tenant_request");
        return;
    }
}

