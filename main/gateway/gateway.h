/**
 * Gateway Layer - Protocol Abstraction Interface
 * 
 * Provides unified protocol abstraction for all communication channels.
 * Gateway handles the transport layer (WS/HTTP/STDIO) while Channel handles business logic.
 */

#ifndef GATEWAY_H
#define GATEWAY_H

#include "mimi_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct gateway gateway_t;

/* Gateway types */
typedef enum {
    GATEWAY_TYPE_STDIO,         /* Standard input/output */
    GATEWAY_TYPE_WS_SERVER,     /* WebSocket server */
    GATEWAY_TYPE_WS_CLIENT,     /* WebSocket client */
    GATEWAY_TYPE_HTTP_CLIENT    /* HTTP client */
} gateway_type_t;

/* Gateway configuration */
typedef struct {
    gateway_type_t type;
    const char *name;           /* Unique identifier, e.g., "stdio", "ws_server", "feishu_ws" */
    
    /* Type-specific configuration */
    union {
        struct {
            /* STDIO has no config */
        } stdio;
        
        struct {
            int port;           /* Listen port */
            const char *path;   /* WebSocket path, e.g., "/" */
        } ws_server;
        
        struct {
            const char *url;            /* WebSocket URL, e.g., "wss://open.feishu.cn/..." */
            const char *token;          /* Authentication token */
            int reconnect_interval_ms;  /* Reconnect interval */
        } ws_client;
        
        struct {
            const char *base_url;       /* Base URL, e.g., "https://api.telegram.org" */
            const char *token;          /* Authentication token */
            int timeout_ms;             /* Request timeout */
        } http;
    } config;
} gateway_config_t;

/* Callback function types */
typedef void (*gateway_on_message_cb_t)(gateway_t *gw, 
                                        const char *session_id, 
                                        const char *content,
                                        size_t content_len,
                                        void *user_data);

typedef void (*gateway_on_connect_cb_t)(gateway_t *gw, 
                                        const char *session_id, 
                                        void *user_data);

typedef void (*gateway_on_disconnect_cb_t)(gateway_t *gw, 
                                           const char *session_id, 
                                           void *user_data);

/**
 * Gateway interface structure
 * All gateway implementations must provide these functions
 */
struct gateway {
    /* Identity */
    char name[32];              /* Gateway name */
    gateway_type_t type;        /* Gateway type */
    
    /* Lifecycle functions */
    mimi_err_t (*init)(gateway_t *gw, const gateway_config_t *cfg);
    mimi_err_t (*start)(gateway_t *gw);
    mimi_err_t (*stop)(gateway_t *gw);
    void (*destroy)(gateway_t *gw);
    
    /* Messaging */
    mimi_err_t (*send)(gateway_t *gw, 
                       const char *session_id, 
                       const char *content);
    
    /* Callback registration */
    void (*set_on_message)(gateway_t *gw, 
                           gateway_on_message_cb_t cb, 
                           void *user_data);
    
    void (*set_on_connect)(gateway_t *gw, 
                           gateway_on_connect_cb_t cb, 
                           void *user_data);
    
    void (*set_on_disconnect)(gateway_t *gw, 
                              gateway_on_disconnect_cb_t cb, 
                              void *user_data);
    
    /* State */
    bool is_initialized;
    bool is_started;
    void *priv_data;            /* Implementation-specific data */
    
    /* Callback storage */
    gateway_on_message_cb_t on_message_cb;
    gateway_on_connect_cb_t on_connect_cb;
    gateway_on_disconnect_cb_t on_disconnect_cb;
    void *callback_user_data;
};

/**
 * Initialize a gateway instance
 * @param gw Gateway instance to initialize
 * @param cfg Configuration
 * @return MIMI_OK on success
 */
mimi_err_t gateway_init(gateway_t *gw, const gateway_config_t *cfg);

/**
 * Start a gateway
 * @param gw Gateway instance
 * @return MIMI_OK on success
 */
mimi_err_t gateway_start(gateway_t *gw);

/**
 * Stop a gateway
 * @param gw Gateway instance
 * @return MIMI_OK on success
 */
mimi_err_t gateway_stop(gateway_t *gw);

/**
 * Destroy a gateway and free resources
 * @param gw Gateway instance
 */
void gateway_destroy(gateway_t *gw);

/**
 * Send message through gateway
 * @param gw Gateway instance
 * @param session_id Session identifier
 * @param content Message content
 * @return MIMI_OK on success
 */
mimi_err_t gateway_send(gateway_t *gw, 
                        const char *session_id, 
                        const char *content);

/**
 * Set message callback
 * @param gw Gateway instance
 * @param cb Callback function
 * @param user_data User data passed to callback
 */
void gateway_set_on_message(gateway_t *gw, 
                            gateway_on_message_cb_t cb, 
                            void *user_data);

/**
 * Set connect callback
 * @param gw Gateway instance
 * @param cb Callback function
 * @param user_data User data passed to callback
 */
void gateway_set_on_connect(gateway_t *gw, 
                            gateway_on_connect_cb_t cb, 
                            void *user_data);

/**
 * Set disconnect callback
 * @param gw Gateway instance
 * @param cb Callback function
 * @param user_data User data passed to callback
 */
void gateway_set_on_disconnect(gateway_t *gw, 
                               gateway_on_disconnect_cb_t cb, 
                               void *user_data);

/**
 * Check if gateway is running
 * @param gw Gateway instance
 * @return true if running
 */
bool gateway_is_running(gateway_t *gw);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_H */
