/**
 * HTTP Gateway Implementation
 *
 * Provides HTTP client functionality using mongoose
 */

#include "gateway/http/http_gateway.h"
#include "log.h"
#include "runtime.h"
#include "http/http.h"
#include "os/os.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "gw_http";

/* Synchronous callback for blocking HTTP requests */
typedef struct {
    mimi_err_t result;
    bool completed;
    mimi_mutex_t *mutex;
    mimi_cond_t *cond;
} sync_callback_data_t;

/* Global HTTP Gateway instance */
static http_gateway_priv_t s_http_priv = {0};
static gateway_t g_http_gateway = {0};

/* HTTP Gateway implementation functions */

static mimi_err_t http_gateway_init_impl(gateway_t *gw, const http_gateway_config_t *cfg)
{
    http_gateway_priv_t *priv = (http_gateway_priv_t *)gw->priv_data;
    
    if (priv->initialized) {
        MIMI_LOGW(TAG, "HTTP Gateway already initialized");
        return MIMI_OK;
    }
    
    /* Store configuration */
    if (cfg->base_url) {
        strncpy(priv->base_url, cfg->base_url, sizeof(priv->base_url) - 1);
    }
    if (cfg->api_token) {
        strncpy(priv->api_token, cfg->api_token, sizeof(priv->api_token) - 1);
    }
    priv->timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 30000;
    
    if (cfg->proxy_host) {
        strncpy(priv->proxy_host, cfg->proxy_host, sizeof(priv->proxy_host) - 1);
        priv->proxy_port = cfg->proxy_port > 0 ? cfg->proxy_port : 8080;
    }
    
    priv->gateway = gw;
    priv->initialized = true;
    priv->connected = true;
    gw->is_initialized = true;
    
    MIMI_LOGI(TAG, "HTTP Gateway initialized (base_url: %s)", priv->base_url);
    return MIMI_OK;
}

static mimi_err_t http_gateway_start_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    http_gateway_priv_t *priv = (http_gateway_priv_t *)gw->priv_data;
    
    if (!priv->initialized) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (priv->connected) {
        return MIMI_OK;
    }
    
    priv->connected = true;
    MIMI_LOGI(TAG, "HTTP Gateway started");
    return MIMI_OK;
}

static mimi_err_t http_gateway_stop_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return MIMI_OK;
    }
    
    http_gateway_priv_t *priv = (http_gateway_priv_t *)gw->priv_data;
    
    if (!priv->connected) {
        return MIMI_OK;
    }
    
    priv->connected = false;
    MIMI_LOGI(TAG, "HTTP Gateway stopped");
    return MIMI_OK;
}

static void http_gateway_destroy_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return;
    }
    
    http_gateway_priv_t *priv = (http_gateway_priv_t *)gw->priv_data;
    
    http_gateway_stop_impl(gw);
    
    memset(priv, 0, sizeof(http_gateway_priv_t));
    MIMI_LOGI(TAG, "HTTP Gateway destroyed");
}

static mimi_err_t http_gateway_send_impl(gateway_t *gw, const char *session_id,
                                           const char *content)
{
    (void)session_id;
    (void)content;
    (void)gw;
    
    /* HTTP Gateway doesn't support direct send - use http_gateway_get/post instead */
    return MIMI_ERR_NOT_SUPPORTED;
}

/* HTTP Gateway module initialization */





static void sync_http_callback(mimi_err_t err, mimi_http_response_t *resp, void *user_data)
{
    sync_callback_data_t *data = (sync_callback_data_t *)user_data;
    if (data) {
        mimi_mutex_lock(data->mutex);
        data->result = err;
        data->completed = true;
        mimi_cond_signal(data->cond);
        mimi_mutex_unlock(data->mutex);
    }
}



mimi_err_t http_gateway_module_init(void)
{
    memset(&s_http_priv, 0, sizeof(s_http_priv));
    
    /* Initialize gateway structure */
    strncpy(g_http_gateway.name, "http", sizeof(g_http_gateway.name) - 1);
    g_http_gateway.type = GATEWAY_TYPE_HTTP_CLIENT;
    g_http_gateway.init = (mimi_err_t (*)(gateway_t *, const gateway_config_t *))http_gateway_init_impl;
    g_http_gateway.start = http_gateway_start_impl;
    g_http_gateway.stop = http_gateway_stop_impl;
    g_http_gateway.destroy = http_gateway_destroy_impl;
    g_http_gateway.send = http_gateway_send_impl;
    g_http_gateway.set_on_message = NULL;
    g_http_gateway.set_on_connect = NULL;
    g_http_gateway.set_on_disconnect = NULL;
    g_http_gateway.priv_data = &s_http_priv;
    g_http_gateway.is_initialized = false;
    g_http_gateway.is_started = false;
    
    MIMI_LOGI(TAG, "HTTP Gateway module initialized");
    
    /* Initialize HTTP platform module */
    mimi_http_init();
    
    return MIMI_OK;
}

gateway_t* http_gateway_get_instance(void)
{
    return &g_http_gateway;
}

mimi_err_t http_gateway_configure(const char *base_url, const char *api_token, int timeout_ms)
{
    http_gateway_config_t cfg = {
        .base_url = base_url,
        .api_token = api_token,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 30000,
        .proxy_host = NULL,
        .proxy_port = 0
    };
    
    return http_gateway_init_impl(&g_http_gateway, &cfg);
}

mimi_err_t http_gateway_get(gateway_t *gw, const char *endpoint,
                           char *response, size_t response_len)
{
    http_gateway_priv_t *priv = (http_gateway_priv_t *)gw->priv_data;
    
    if (!priv->initialized || !priv->connected) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!endpoint || !response || response_len == 0) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Build full URL */
    char url[512];
    snprintf(url, sizeof(url), "%s%s", priv->base_url, endpoint);
    
    /* Prepare HTTP request */
    mimi_http_request_t req = {
        .method = "GET",
        .url = url,
        .headers = priv->api_token[0] ? 
                 "Authorization: Bearer " : NULL,
        .body = NULL,
        .body_len = 0,
        .timeout_ms = priv->timeout_ms
    };
    
    /* Add Authorization header if token is set */
    char headers[256] = {0};
    if (priv->api_token[0]) {
        snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", priv->api_token);
        req.headers = headers;
    }
    
    /* Execute HTTP request asynchronously but wait for completion */
    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    
    sync_callback_data_t sync_data;
    sync_data.result = MIMI_ERR_FAIL;
    sync_data.completed = false;
    
    mimi_err_t mutex_err = mimi_mutex_create(&sync_data.mutex);
    mimi_err_t cond_err = mimi_cond_create(&sync_data.cond);
    if (mutex_err != MIMI_OK || cond_err != MIMI_OK) {
        if (sync_data.mutex) {
            mimi_mutex_destroy(sync_data.mutex);
        }
        if (sync_data.cond) {
            mimi_cond_destroy(sync_data.cond);
        }
        return MIMI_ERR_FAIL;
    }
    
    mimi_err_t err = mimi_http_exec_async(&req, &resp, sync_http_callback, &sync_data);
    if (err != MIMI_OK) {
        mimi_mutex_destroy(sync_data.mutex);
        mimi_cond_destroy(sync_data.cond);
        return err;
    }
    
    /* Wait for callback */
    mimi_mutex_lock(sync_data.mutex);
    while (!sync_data.completed) {
        mimi_cond_wait(sync_data.cond, sync_data.mutex, 60000); // 60s timeout
    }
    err = sync_data.result;
    mimi_mutex_unlock(sync_data.mutex);
    
    mimi_mutex_destroy(sync_data.mutex);
    mimi_cond_destroy(sync_data.cond);
    
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "HTTP GET request failed: %d", err);
        mimi_http_response_free(&resp);
        return err;
    }
    
    /* Copy response */
    if (resp.body) {
        size_t copy_len = resp.body_len;
        if (copy_len > response_len - 1) copy_len = response_len - 1;
        memcpy(response, resp.body, copy_len);
        response[copy_len] = '\0';
        mimi_http_response_free(&resp);
    } else {
        response[0] = '\0';
        mimi_http_response_free(&resp);
        return MIMI_ERR_IO;
    }
    
    return MIMI_OK;
}

mimi_err_t http_gateway_post(gateway_t *gw, const char *endpoint,
                            const char *data, size_t data_len,
                            char *response, size_t response_len)
{
    http_gateway_priv_t *priv = (http_gateway_priv_t *)gw->priv_data;
    
    if (!priv->initialized || !priv->connected) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!endpoint || !data || !response || response_len == 0) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Build full URL */
    char url[512];
    snprintf(url, sizeof(url), "%s%s", priv->base_url, endpoint);
    
    /* Add Authorization header if token is set */
    char headers[256] = {0};
    if (priv->api_token[0]) {
        snprintf(headers, sizeof(headers), 
                 "Authorization: Bearer %s\r\n" 
                 "Content-Type: application/json\r\n", 
                 priv->api_token);
    } else {
        strcpy(headers, "Content-Type: application/json\r\n");
    }
    
    /* Prepare HTTP request */
    mimi_http_request_t req = {
        .method = "POST",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)data,
        .body_len = data_len,
        .timeout_ms = priv->timeout_ms
    };
    
    /* Execute HTTP request asynchronously but wait for completion */
    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    
    sync_callback_data_t sync_data;
    sync_data.result = MIMI_ERR_FAIL;
    sync_data.completed = false;
    
    mimi_err_t mutex_err = mimi_mutex_create(&sync_data.mutex);
    mimi_err_t cond_err = mimi_cond_create(&sync_data.cond);
    if (mutex_err != MIMI_OK || cond_err != MIMI_OK) {
        if (sync_data.mutex) {
            mimi_mutex_destroy(sync_data.mutex);
        }
        if (sync_data.cond) {
            mimi_cond_destroy(sync_data.cond);
        }
        return MIMI_ERR_FAIL;
    }
    
    mimi_err_t err = mimi_http_exec_async(&req, &resp, sync_http_callback, &sync_data);
    if (err != MIMI_OK) {
        mimi_mutex_destroy(sync_data.mutex);
        mimi_cond_destroy(sync_data.cond);
        return err;
    }
    
    /* Wait for callback */
    mimi_mutex_lock(sync_data.mutex);
    while (!sync_data.completed) {
        mimi_cond_wait(sync_data.cond, sync_data.mutex, 60000); // 60s timeout
    }
    err = sync_data.result;
    mimi_mutex_unlock(sync_data.mutex);
    
    mimi_mutex_destroy(sync_data.mutex);
    mimi_cond_destroy(sync_data.cond);
    
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "HTTP POST request failed: %d", err);
        mimi_http_response_free(&resp);
        return err;
    }
    
    /* Copy response */
    if (resp.body) {
        size_t copy_len = resp.body_len;
        if (copy_len > response_len - 1) copy_len = response_len - 1;
        memcpy(response, resp.body, copy_len);
        response[copy_len] = '\0';
        mimi_http_response_free(&resp);
    } else {
        response[0] = '\0';
        mimi_http_response_free(&resp);
        return MIMI_ERR_IO;
    }
    
    return MIMI_OK;
}

bool http_gateway_is_connected(gateway_t *gw)
{
    http_gateway_priv_t *priv = (http_gateway_priv_t *)gw->priv_data;
    return priv && priv->connected;
}