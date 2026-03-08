/**
 * STDIO Gateway - Standard Input/Output transport implementation
 * 
 * Provides terminal-based input/output for CLI channel
 */

#ifndef STDIO_GATEWAY_H
#define STDIO_GATEWAY_H

#include "gateway/gateway.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * STDIO Gateway instance
 * Use this to register with Gateway Manager
 */
extern gateway_t g_stdio_gateway;

/**
 * Initialize STDIO Gateway module
 * Call before registering with Gateway Manager
 * @return MIMI_OK on success
 */
mimi_err_t stdio_gateway_module_init(void);

/**
 * Get STDIO Gateway instance
 * @return Gateway instance or NULL if not initialized
 */
gateway_t* stdio_gateway_get(void);

#ifdef __cplusplus
}
#endif

#endif /* STDIO_GATEWAY_H */
