/**
 * HTTP Client Gateway Implementation
 *
 * Provides HTTP client functionality using mongoose.
 *
 * Design: All request parameters are passed via http_request_options_t for
 * full reentrancy and thread-safety. No global state is modified during requests.
 */

#include "gateway/http/http_client_gateway.h"
#include "log.h"
#include "runtime.h"
#include "http/http.h"
#include "os/os.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "gw_http";

/* HTTP Client Gateway private data */
typedef struct {
    char base_url[256];
    char api_token[128];
    int timeout_ms;
    char proxy_host[128];
    int proxy_port;
    bool initialized;
    bool connected;
    gateway_t *gateway;
} http_client_gateway_priv_t;

/* Global HTTP Client Gateway instance */
static http_client_gateway_priv_t s_http_priv = {0};
static gateway_t g_http_gateway = {0};

/* HTTP Client Gateway implementation functions */

static mimi_err_t http_client_gateway_init_impl(gateway_t *gw, const gateway_config_t *cfg)
{
    http_client_gateway_priv_t *priv = (http_client_gateway_priv_t *)gw->priv_data;
    
    priv->base_url[0] = '\0';
    priv->api_token[0] = '\0';
    priv->timeout_ms = 30000;
    
    /* Optional defaults from unified gateway_config_t (per-request opts still override). */
    if (cfg && cfg->type == GATEWAY_TYPE_HTTP_CLIENT) {
        if (cfg->config.http.base_url && cfg->config.http.base_url[0]) {
            strncpy(priv->base_url, cfg->config.http.base_url, sizeof(priv->base_url) - 1);
            priv->base_url[sizeof(priv->base_url) - 1] = '\0';
        }
        if (cfg->config.http.token && cfg->config.http.token[0]) {
            strncpy(priv->api_token, cfg->config.http.token, sizeof(priv->api_token) - 1);
            priv->api_token[sizeof(priv->api_token) - 1] = '\0';
        }
        if (cfg->config.http.timeout_ms > 0) {
            priv->timeout_ms = cfg->config.http.timeout_ms;
        }
    }
    
    priv->gateway = gw;
    priv->initialized = true;
    priv->connected = true;
    /* gw->is_initialized is set only by gateway_init() after this returns. */
    
    MIMI_LOGD(TAG, "HTTP Gateway initialized");
    return MIMI_OK;
}

static mimi_err_t http_client_gateway_start_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    http_client_gateway_priv_t *priv = (http_client_gateway_priv_t *)gw->priv_data;
    
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

static mimi_err_t http_client_gateway_stop_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return MIMI_OK;
    }
    
    http_client_gateway_priv_t *priv = (http_client_gateway_priv_t *)gw->priv_data;
    
    if (!priv->connected) {
        return MIMI_OK;
    }
    
    priv->connected = false;
    MIMI_LOGI(TAG, "HTTP Gateway stopped");
    return MIMI_OK;
}

static void http_client_gateway_destroy_impl(gateway_t *gw)
{
    if (!gw || !gw->priv_data) {
        return;
    }
    
    http_client_gateway_priv_t *priv = (http_client_gateway_priv_t *)gw->priv_data;
    
    http_client_gateway_stop_impl(gw);
    
    memset(priv, 0, sizeof(http_client_gateway_priv_t));
    MIMI_LOGI(TAG, "HTTP Gateway destroyed");
}

static mimi_err_t http_client_gateway_send_impl(gateway_t *gw, const char *session_id,
                                                const char *content)
{
    (void)session_id;
    (void)content;
    (void)gw;
    
    /* HTTP Gateway doesn't support direct send - use http_client_gateway_get/post instead */
    return MIMI_ERR_NOT_SUPPORTED;
}

/* HTTP Client Gateway module initialization */

mimi_err_t http_client_gateway_module_init(void)
{
    memset(&s_http_priv, 0, sizeof(s_http_priv));
    
    /* Initialize gateway structure */
    strncpy(g_http_gateway.name, "http", sizeof(g_http_gateway.name) - 1);
    g_http_gateway.type = GATEWAY_TYPE_HTTP_CLIENT;
    g_http_gateway.init = (mimi_err_t (*)(gateway_t *, const gateway_config_t *))http_client_gateway_init_impl;
    g_http_gateway.start = http_client_gateway_start_impl;
    g_http_gateway.stop = http_client_gateway_stop_impl;
    g_http_gateway.destroy = http_client_gateway_destroy_impl;
    g_http_gateway.send = http_client_gateway_send_impl;
    g_http_gateway.set_on_message = NULL;
    g_http_gateway.set_on_connect = NULL;
    g_http_gateway.set_on_disconnect = NULL;
    g_http_gateway.priv_data = &s_http_priv;
    g_http_gateway.is_initialized = false;
    g_http_gateway.is_started = false;
    
    MIMI_LOGD(TAG, "HTTP Gateway module initialized");
    
    /* Initialize HTTP platform module */
    mimi_http_init();
    
    return MIMI_OK;
}

gateway_t* http_client_gateway_get_instance(void)
{
    return &g_http_gateway;
}

/**
 * Internal helper to build URL and headers from options
 */
static mimi_err_t build_request_context(http_client_gateway_priv_t *priv,
                                        const http_request_options_t *opts,
                                        const char *endpoint,
                                        char *url, size_t url_size,
                                        char *headers, size_t headers_size,
                                        int *timeout_ms,
                                        bool is_post)
{
    /* Build URL: use opts->base_url if provided, else priv->base_url */
    const char *base = (opts && opts->base_url) ? opts->base_url : priv->base_url;
    if (!base || !base[0]) {
        MIMI_LOGE(TAG, "HTTP request missing base_url (pass opts->base_url or init with gateway_config_t.http)");
        return MIMI_ERR_INVALID_ARG;
    }
    if (!endpoint || !endpoint[0]) {
        MIMI_LOGE(TAG, "HTTP request missing endpoint");
        return MIMI_ERR_INVALID_ARG;
    }
    snprintf(url, url_size, "%s%s", base, endpoint);
    
    /* Build headers */
    int offset = 0;
    
    /* Authorization: use opts->auth_token if provided, else priv->api_token */
    const char *token = (opts && opts->auth_token) ? opts->auth_token : priv->api_token;
    if (token && token[0]) {
        offset += snprintf(headers + offset, headers_size - offset,
                          "Authorization: Bearer %s\r\n", token);
    }
    
    /* Content-Type for POST */
    if (is_post) {
        offset += snprintf(headers + offset, headers_size - offset,
                          "Content-Type: application/json\r\n");
    }
    
    /* Extra headers from opts */
    if (opts && opts->extra_headers && opts->extra_headers[0]) {
        offset += snprintf(headers + offset, headers_size - offset,
                          "%s\r\n", opts->extra_headers);
    }
    
    /* Timeout: use opts->timeout_ms if provided and > 0, else priv->timeout_ms */
    *timeout_ms = (opts && opts->timeout_ms > 0) ? opts->timeout_ms : priv->timeout_ms;
    return MIMI_OK;
}

mimi_err_t http_client_gateway_get(gateway_t *gw, const char *endpoint,
                                   const http_request_options_t *opts,
                                   char *response, size_t response_len)
{
    if (!gw) {
        return MIMI_ERR_INVALID_ARG;
    }
    http_client_gateway_priv_t *priv = (http_client_gateway_priv_t *)gw->priv_data;
    if (!priv) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (!priv->initialized || !priv->connected) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!endpoint || !response || response_len == 0) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Build URL and headers */
    char url[512];
    char headers[768] = {0};
    int timeout_ms;
    
    mimi_err_t build_err = build_request_context(priv, opts, endpoint, url, sizeof(url),
                          headers, sizeof(headers), &timeout_ms, false);
    if (build_err != MIMI_OK) {
        return build_err;
    }
    
    /* Prepare HTTP request */
    mimi_http_request_t req = {
        .method = "GET",
        .url = url,
        .headers = headers[0] ? headers : NULL,
        .body = NULL,
        .body_len = 0,
        .timeout_ms = timeout_ms
    };
    
    /* Execute HTTP request synchronously */
    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    
    mimi_err_t err = mimi_http_exec(&req, &resp);
    
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

mimi_err_t http_client_gateway_post(gateway_t *gw, const char *endpoint,
                                    const http_request_options_t *opts,
                                    const char *data, size_t data_len,
                                    char *response, size_t response_len)
{
    http_client_gateway_priv_t *priv = (http_client_gateway_priv_t *)gw->priv_data;
    
    if (!gw || !priv) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (!priv->initialized || !priv->connected) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!endpoint || !data || !response || response_len == 0) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Build URL and headers */
    char url[512];
    char headers[768] = {0};
    int timeout_ms;
    
    mimi_err_t build_err = build_request_context(priv, opts, endpoint, url, sizeof(url),
                          headers, sizeof(headers), &timeout_ms, true);
    if (build_err != MIMI_OK) {
        return build_err;
    }
    
    /* Prepare HTTP request */
    mimi_http_request_t req = {
        .method = "POST",
        .url = url,
        .headers = headers,
        .body = (const uint8_t *)data,
        .body_len = data_len,
        .timeout_ms = timeout_ms
    };
    
    /* Execute HTTP request synchronously */
    mimi_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    
    mimi_err_t err = mimi_http_exec(&req, &resp);
    
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

bool http_client_gateway_is_connected(gateway_t *gw)
{
    http_client_gateway_priv_t *priv = (http_client_gateway_priv_t *)gw->priv_data;
    return priv && priv->initialized && priv->connected;
}
