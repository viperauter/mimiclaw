/**
 * Router Interface
 * 
 * Unified routing for commands and chat messages from all gateways
 */

#ifndef ROUTER_H
#define ROUTER_H

#include "gateway/gateway.h"
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTER_MAX_MAPPINGS 16
#define ROUTER_OUTPUT_MAX_LEN 4096

/**
 * Initialize router
 */
mimi_err_t router_init(void);

/**
 * Register gateway to channel mapping
 */
mimi_err_t router_register_mapping(const char *gateway_name, 
                                   const char *channel_name);

/**
 * Unregister gateway mapping
 */
mimi_err_t router_unregister_mapping(const char *gateway_name);

/**
 * Find channel for gateway
 */
const char* router_find_channel(const char *gateway_name);

/**
 * Check if content is a command
 */
bool router_is_command(const char *content);

/**
 * Handle input from gateway
 */
mimi_err_t router_handle(gateway_t *gw, 
                         const char *session_id, 
                         const char *content);

/**
 * Handle Telegram input
 */
mimi_err_t router_handle_telegram(const char *session_id,
                                  const char *content);

/**
 * Handle Feishu input
 */
mimi_err_t router_handle_feishu(const char *session_id,
                                const char *content);

/**
 * Handle QQ input
 */
mimi_err_t router_handle_qq(const char *session_id,
                            const char *content);

/**
 * Handle WeChat input
 */
mimi_err_t router_handle_wechat(const char *session_id,
                                const char *content);

/**
 * Generic channel input handler - dynamically routed by channel_name
 * Eliminates need for per-channel handler boilerplate
 * 
 * @param channel_name Name of the source channel (e.g., "telegram", "wechat")
 * @param session_id Unique session/chat identifier
 * @param content Message content
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t router_handle_generic(const char *channel_name,
                                 const char *session_id,
                                 const char *content);

#ifdef __cplusplus
}
#endif

#endif /* ROUTER_H */