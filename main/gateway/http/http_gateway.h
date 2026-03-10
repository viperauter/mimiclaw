/**
 * HTTP Gateway Interface
 *
 * Provides HTTP client functionality for channels that need HTTP communication.
 * Used by Telegram, Feishu, QQ and other HTTP-based channels.
 */

#ifndef HTTP_GATEWAY_H
#define HTTP_GATEWAY_H

#include "gateway/gateway.h"
#include "mimi_err.h"
#include "http/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP Gateway configuration */
typedef struct {
    const char *base_url;           /* Base URL for API requests */
    const char *api_token;          /* API token for authentication */
    int timeout_ms;                 /* Request timeout in milliseconds */
    const char *proxy_host;          /* Proxy host (optional) */
    int proxy_port;                 /* Proxy port (optional) */
} http_gateway_config_t;

/* HTTP Gateway private data */
typedef struct {
    char base_url[256];
    char api_token[128];
    int timeout_ms;
    
    /* Proxy settings */
    char proxy_host[128];
    int proxy_port;
    
    /* Connection state */
    bool initialized;
    bool connected;
    
    /* Gateway reference */
    gateway_t *gateway;
} http_gateway_priv_t;

/**
 * Initialize HTTP Gateway
 * @param gw Gateway object
 * @param cfg HTTP Gateway configuration
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t http_gateway_init(gateway_t *gw, const http_gateway_config_t *cfg);

/**
 * Send HTTP GET request
 * @param gw Gateway object
 * @param endpoint API endpoint (relative to base_url)
 * @param response Buffer for response
 * @param response_len Size of response buffer
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t http_gateway_get(gateway_t *gw, const char *endpoint,
                           char *response, size_t response_len);

/**
 * Send HTTP POST request
 * @param gw Gateway object
 * @param endpoint API endpoint (relative to base_url)
 * @param data Request body data
 * @param data_len Length of request body
 * @param response Buffer for response
 * @param response_len Size of response buffer
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t http_gateway_post(gateway_t *gw, const char *endpoint,
                            const char *data, size_t data_len,
                            char *response, size_t response_len);

/**
 * Check if HTTP Gateway is connected
 * @param gw Gateway object
 * @return true if connected, false otherwise
 */
bool http_gateway_is_connected(gateway_t *gw);

/**
 * HTTP Gateway module initialization
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t http_gateway_module_init(void);

/**
 * Get HTTP Gateway instance
 * @return Pointer to HTTP Gateway object
 */
gateway_t* http_gateway_get_instance(void);

/**
 * Configure HTTP Gateway
 * @param base_url Base URL for API requests
 * @param api_token API token for authentication
 * @param timeout_ms Request timeout in milliseconds
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t http_gateway_configure(const char *base_url, const char *api_token, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_GATEWAY_H */
