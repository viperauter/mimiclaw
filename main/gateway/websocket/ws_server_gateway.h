/**
 * WebSocket Server Gateway - WebSocket server transport implementation
 *
 * Provides WebSocket server transport for channels.
 */

#ifndef WS_SERVER_GATEWAY_H
#define WS_SERVER_GATEWAY_H

#include "gateway/gateway.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WebSocket Server Gateway configuration
 */
typedef struct {
    int port;
    const char *path;
} ws_gateway_config_t;

/**
 * WebSocket Server Gateway instance
 */
extern gateway_t g_ws_gateway;

/**
 * Initialize WebSocket Server Gateway module
 * @return MIMI_OK on success
 */
mimi_err_t ws_gateway_module_init(void);

/**
 * Get WebSocket Server Gateway instance
 * @return Gateway instance or NULL if not initialized
 */
gateway_t* ws_gateway_get(void);

/**
 * Configure WebSocket Server Gateway before starting
 * @param port Server port
 * @param path WebSocket path (e.g., "/")
 * @return MIMI_OK on success
 */
mimi_err_t ws_gateway_configure(int port, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* WS_SERVER_GATEWAY_H */

