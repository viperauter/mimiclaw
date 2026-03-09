/**
 * WebSocket Client Gateway
 *
 * Provides WebSocket client functionality for channels that need long connections.
 * Used by Feishu, QQ and other WebSocket-based channels.
 */

#ifndef WS_CLIENT_GATEWAY_H
#define WS_CLIENT_GATEWAY_H

#include "gateway/gateway.h"
#include "mimi_err.h"
#include "platform/websocket/websocket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WebSocket Client Gateway configuration */
typedef struct {
    const char *url;               /* WebSocket server URL */
    const char *api_token;          /* API token for authentication */
    int timeout_ms;                 /* Connection timeout in milliseconds */
    int ping_interval_ms;            /* Heartbeat ping interval */
} ws_client_gateway_config_t;

/* WebSocket Client Gateway private data */
typedef struct {
    char url[256];
    char api_token[128];
    char auth_header[256];         /* Full Authorization header */
    int timeout_ms;
    int ping_interval_ms;
    
    /* Connection state */
    bool initialized;
    bool connected;
    bool reconnecting;
    bool stopping;                 /* Set when gateway is being stopped */
    
    /* Platform WebSocket */
    mimi_websocket_t *ws;
    
    /* Gateway reference */
    gateway_t *gateway;
    
    /* Channel-specific data */
    void *channel_data;
} ws_client_gateway_priv_t;

/**
 * Initialize WebSocket Client Gateway
 * @param gw Gateway object
 * @param cfg WebSocket Client Gateway configuration
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t ws_client_gateway_init(gateway_t *gw, const ws_client_gateway_config_t *cfg);

/**
 * Connect to WebSocket server
 * @param gw Gateway object
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t ws_client_gateway_connect(gateway_t *gw);

/**
 * Disconnect from WebSocket server
 * @param gw Gateway object
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t ws_client_gateway_disconnect(gateway_t *gw);

/**
 * Send message through WebSocket
 * @param gw Gateway object
 * @param session_id Session ID (or NULL for broadcast)
 * @param content Message content
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t ws_client_gateway_send(gateway_t *gw, const char *session_id,
                                  const char *content);

/**
 * Check if WebSocket Client Gateway is connected
 * @param gw Gateway object
 * @return true if connected, false otherwise
 */
bool ws_client_gateway_is_connected(gateway_t *gw);

/**
 * Set channel-specific data
 * @param gw Gateway object
 * @param data Channel-specific data pointer
 */
void ws_client_gateway_set_channel_data(gateway_t *gw, void *data);

/**
 * Get channel-specific data
 * @param gw Gateway object
 * @return Channel-specific data pointer
 */
void* ws_client_gateway_get_channel_data(gateway_t *gw);

/**
 * WebSocket Client Gateway module initialization
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t ws_client_gateway_module_init(void);

/**
 * Get WebSocket Client Gateway instance
 * @return Pointer to WebSocket Client Gateway object
 */
gateway_t* ws_client_gateway_get_instance(void);

/**
 * Configure WebSocket Client Gateway
 * @param url WebSocket server URL
 * @param api_token API token for authentication
 * @param timeout_ms Connection timeout in milliseconds
 * @param ping_interval_ms Heartbeat ping interval in milliseconds
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t ws_client_gateway_configure(const char *url, const char *api_token,
                                     int timeout_ms, int ping_interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* WS_CLIENT_GATEWAY_H */
