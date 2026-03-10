/**
 * HTTP Client Gateway Interface
 *
 * Provides HTTP client functionality for channels that need HTTP communication.
 * Used by Telegram, Feishu, QQ and other HTTP-based channels.
 */

#ifndef HTTP_CLIENT_GATEWAY_H
#define HTTP_CLIENT_GATEWAY_H

#include "gateway/gateway.h"
#include "mimi_err.h"
#include "http/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP Client Gateway configuration */
typedef struct {
    const char *base_url;           /* Base URL for API requests */
    const char *api_token;          /* API token for authentication */
    int timeout_ms;                 /* Request timeout in milliseconds */
    const char *proxy_host;         /* Proxy host (optional) */
    int proxy_port;                 /* Proxy port (optional) */
} http_client_gateway_config_t;

/* HTTP Client Gateway private data */
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
} http_client_gateway_priv_t;

/**
 * Initialize HTTP Client Gateway
 * @param gw Gateway object
 * @param cfg HTTP Gateway configuration
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t http_client_gateway_init(gateway_t *gw, const http_client_gateway_config_t *cfg);

/**
 * Send HTTP GET request
 */
mimi_err_t http_client_gateway_get(gateway_t *gw, const char *endpoint,
                                   char *response, size_t response_len);

/**
 * Send HTTP POST request
 */
mimi_err_t http_client_gateway_post(gateway_t *gw, const char *endpoint,
                                    const char *data, size_t data_len,
                                    char *response, size_t response_len);

/**
 * Check if HTTP Client Gateway is connected
 */
bool http_client_gateway_is_connected(gateway_t *gw);

/**
 * HTTP Client Gateway module initialization
 */
mimi_err_t http_client_gateway_module_init(void);

/**
 * Get HTTP Client Gateway instance
 */
gateway_t* http_client_gateway_get_instance(void);

/**
 * Configure HTTP Client Gateway
 */
mimi_err_t http_client_gateway_configure(const char *base_url, const char *api_token, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_GATEWAY_H */

