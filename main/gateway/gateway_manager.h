/**
 * Gateway Manager - Lifecycle and registry management for all gateways
 */

#ifndef GATEWAY_MANAGER_H
#define GATEWAY_MANAGER_H

#include "gateway/gateway.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GATEWAY_MAX_COUNT 8

/**
 * Initialize gateway manager
 * Must be called before any gateway operations
 * @return MIMI_OK on success
 */
mimi_err_t gateway_manager_init(void);

/**
 * Register a gateway
 * @param gw Gateway instance (must be statically allocated or managed by caller)
 * @return MIMI_OK on success, MIMI_ERR_NO_MEM if max gateways reached
 */
mimi_err_t gateway_manager_register(gateway_t *gw);

/**
 * Unregister a gateway
 * @param name Gateway name
 * @return MIMI_OK on success
 */
mimi_err_t gateway_manager_unregister(const char *name);

/**
 * Find a gateway by name
 * @param name Gateway name
 * @return Gateway instance or NULL if not found
 */
gateway_t* gateway_manager_find(const char *name);

/**
 * Start all registered gateways
 * @return MIMI_OK on success
 */
mimi_err_t gateway_manager_start_all(void);

/**
 * Stop all registered gateways
 */
void gateway_manager_stop_all(void);

/**
 * Destroy all registered gateways and cleanup
 */
void gateway_manager_destroy_all(void);

/**
 * Get count of registered gateways
 * @return Number of registered gateways
 */
int gateway_manager_count(void);

/**
 * Iterate over all gateways
 * @param callback Function called for each gateway
 * @param user_data Passed to callback
 */
void gateway_manager_foreach(void (*callback)(gateway_t *gw, void *user_data), 
                             void *user_data);

/* Gateway System Level Functions */

/**
 * Initialize gateway system
 * Registers and initializes all built-in gateways
 * @return MIMI_OK on success
 */
mimi_err_t gateway_system_init(void);

/**
 * Start gateway system
 * Starts all registered gateways
 * @return MIMI_OK on success
 */
mimi_err_t gateway_system_start(void);

/**
 * Stop gateway system
 * Stops all registered gateways
 */
void gateway_system_stop(void);

/**
 * Deinitialize gateway system
 * Destroys all gateways and cleanup
 */
void gateway_system_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_MANAGER_H */
