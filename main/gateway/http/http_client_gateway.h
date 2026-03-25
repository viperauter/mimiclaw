/**
 * HTTP Client Gateway Interface
 *
 * Provides HTTP client functionality for channels that need HTTP communication.
 * Used by Telegram, Feishu, QQ, WeChat and other HTTP-based channels.
 *
 * Design: All request parameters are passed via http_request_options_t for
 * full reentrancy and thread-safety. No global state is modified during requests.
 *
 * Usage:
 *   1. Gateway is initialized via gateway_system_init() -> gateway_init(NULL), or pass
 *      gateway_config_t with type GATEWAY_TYPE_HTTP_CLIENT for default base_url/token/timeout.
 *   2. Use http_client_gateway_get/post() with options for each request (required when
 *      no defaults were set at init).
 *   3. Options can use convenience macros: HTTP_OPTS_BASE(), HTTP_OPTS_TOKEN(), etc.
 */

#ifndef HTTP_CLIENT_GATEWAY_H
#define HTTP_CLIENT_GATEWAY_H

#include "gateway/gateway.h"
#include "mimi_err.h"
#include "http/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Per-request options (fully reentrant)
 * All fields are optional - NULL/0 values will use gateway defaults
 */
typedef struct {
    const char *base_url;      /* Request-level base_url */
    const char *auth_token;    /* Request-level auth token */
    const char *extra_headers; /* Extra headers: "X-Custom: value\r\n" */
    int timeout_ms;            /* Timeout in ms, 0 uses default (30000ms) */
} http_request_options_t;

/**
 * Convenience macros for common usage patterns
 */
#define HTTP_OPTS_DEFAULT          ((http_request_options_t*)NULL)
#define HTTP_OPTS_BASE(url)        (&(http_request_options_t){.base_url = (url)})
#define HTTP_OPTS_TOKEN(tok)       (&(http_request_options_t){.auth_token = (tok)})
#define HTTP_OPTS_BASE_TOKEN(url, tok) \
    (&(http_request_options_t){.base_url = (url), .auth_token = (tok)})

/**
 * Send HTTP GET request
 * @param gw Gateway object (from gateway_manager_find("http"))
 * @param endpoint API endpoint (appended to base_url)
 * @param opts Request options, NULL uses all defaults
 * @param response Buffer for response body
 * @param response_len Size of response buffer
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t http_client_gateway_get(gateway_t *gw, const char *endpoint,
                                   const http_request_options_t *opts,
                                   char *response, size_t response_len);

/**
 * Send HTTP POST request
 * @param gw Gateway object (from gateway_manager_find("http"))
 * @param endpoint API endpoint (appended to base_url)
 * @param opts Request options, NULL uses all defaults
 * @param data Request body data
 * @param data_len Length of request body
 * @param response Buffer for response body
 * @param response_len Size of response buffer
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t http_client_gateway_post(gateway_t *gw, const char *endpoint,
                                    const http_request_options_t *opts,
                                    const char *data, size_t data_len,
                                    char *response, size_t response_len);

/**
 * HTTP Client Gateway module initialization
 * Called automatically by gateway_system_init()
 */
mimi_err_t http_client_gateway_module_init(void);

/**
 * Get HTTP Client Gateway singleton instance
 * @return HTTP Gateway instance
 */
gateway_t* http_client_gateway_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_GATEWAY_H */
