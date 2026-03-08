/**
 * WebSocket Gateway - WebSocket transport implementation
 *
 * Provides WebSocket server transport for channels
 */

#ifndef WS_GATEWAY_H
#define WS_GATEWAY_H

#include "gateway/gateway.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WebSocket Gateway configuration
 */
typedef struct {
    int port;
    const char *path;
} ws_gateway_config_t;

/**
 * WebSocket Gateway instance
 */
extern gateway_t g_ws_gateway;

/**
 * Initialize WebSocket Gateway module
 * @return MIMI_OK on success
 */
mimi_err_t ws_gateway_module_init(void);

/**
 * Get WebSocket Gateway instance
 * @return Gateway instance or NULL if not initialized
 */
gateway_t* ws_gateway_get(void);

/**
 * Configure WebSocket Gateway before starting
 * @param port Server port
 * @param path WebSocket path (e.g., "/")
 * @return MIMI_OK on success
 */
mimi_err_t ws_gateway_configure(int port, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* WS_GATEWAY_H */
