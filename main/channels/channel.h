/**
 * Channel Interface Definition
 * 
 * Channel is the unified message access layer, responsible for:
 * 1. Protocol access (stdio, Telegram, WebSocket, etc.)
 * 2. Message sending and receiving
 * 3. Session management
 */

#ifndef CHANNEL_H
#define CHANNEL_H

#include "mimi_err.h"
#include "bus/message_bus.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of channels */
#define CHANNEL_MAX_COUNT 8

/* Maximum channel name length */
#define CHANNEL_NAME_MAX_LEN 32

/* Maximum session ID length */
#define CHANNEL_SESSION_ID_MAX_LEN 64

/* Forward declaration */
struct channel;
typedef struct channel channel_t;

/**
 * Channel configuration
 */
typedef struct {
    const char *name;           /* Channel name, e.g., "cli", "telegram" */
    const char *description;    /* Channel description */
    bool require_auth;          /* Whether authentication is required */
    int max_sessions;           /* Maximum number of sessions, -1 for unlimited */
    void *user_data;            /* User data */
} channel_config_t;

/**
 * Channel interface
 * 
 * Each channel implementation (CLI, Telegram, WebSocket) must implement this interface
 */
struct channel {
    /* Channel information */
    char name[CHANNEL_NAME_MAX_LEN];
    char description[64];
    bool require_auth;
    int max_sessions;
    
    /* Lifecycle callbacks */
    
    /**
     * Initialize channel
     * @param ch Channel object
     * @param cfg Configuration
     * @return MIMI_OK on success, error code otherwise
     */
    mimi_err_t (*init)(channel_t *ch, const channel_config_t *cfg);
    
    /**
     * Start channel
     * @param ch Channel object
     * @return MIMI_OK on success, error code otherwise
     */
    mimi_err_t (*start)(channel_t *ch);
    
    /**
     * Stop channel
     * @param ch Channel object
     * @return MIMI_OK on success, error code otherwise
     */
    mimi_err_t (*stop)(channel_t *ch);
    
    /**
     * Destroy channel
     * @param ch Channel object
     */
    void (*destroy)(channel_t *ch);
    
    /* Message sending and receiving */
    
    /**
     * Send message to client
     * @param ch Channel object
     * @param session_id Session ID
     * @param content Message content
     * @return MIMI_OK on success, error code otherwise
     */
    mimi_err_t (*send)(channel_t *ch, const char *session_id, 
                       const char *content);
    
    /**
     * Check if channel is running
     * @param ch Channel object
     * @return true if running, false otherwise
     */
    bool (*is_running)(channel_t *ch);
    
    /* Callback settings (optional) */
    
    /**
     * Set callback for received messages
     * @param ch Channel object
     * @param cb Callback function
     * @param user_data User data
     */
    void (*set_on_message)(channel_t *ch, 
                           void (*cb)(channel_t *, const char *session_id, 
                                     const char *content, void *user_data),
                           void *user_data);
    
    /**
     * Set callback for connection established
     * @param ch Channel object
     * @param cb Callback function
     * @param user_data User data
     */
    void (*set_on_connect)(channel_t *ch,
                           void (*cb)(channel_t *, const char *session_id,
                                     void *user_data),
                           void *user_data);
    
    /**
     * Set callback for connection disconnected
     * @param ch Channel object
     * @param cb Callback function
     * @param user_data User data
     */
    void (*set_on_disconnect)(channel_t *ch,
                              void (*cb)(channel_t *, const char *session_id,
                                        void *user_data),
                              void *user_data);
    
    /* Control message handling */
    
    /**
     * Send control request to client
     * @param ch Channel object
     * @param session_id Session ID
     * @param control_type Control message type
     * @param request_id Unique request ID
     * @param target Target of the control (e.g., tool name)
     * @param data Additional data for the control
     * @return MIMI_OK on success, error code otherwise
     */
    mimi_err_t (*send_control)(channel_t *ch, const char *session_id,
                               mimi_control_type_t control_type,
                               const char *request_id,
                               const char *target,
                               const char *data);
    
    /**
     * Set callback for control responses
     * @param ch Channel object
     * @param cb Callback function
     * @param user_data User data
     */
    void (*set_on_control_response)(channel_t *ch,
                                    void (*cb)(channel_t *, const char *session_id,
                                              const char *request_id,
                                              const char *response,
                                              void *user_data),
                                    void *user_data);
    
    /* Internal data (used by channel implementation) */
    void *priv_data;
    bool is_initialized;
    bool is_started;
};

/**
 * Channel manager API
 */

/**
 * Initialize channel manager
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t channel_manager_init(void);

/**
 * Deinitialize channel manager
 */
void channel_manager_deinit(void);

/**
 * Register channel
 * @param ch Channel object
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t channel_register(channel_t *ch);

/**
 * Unregister channel
 * @param ch Channel object
 */
void channel_unregister(channel_t *ch);

/**
 * Find channel
 * @param name Channel name
 * @return Channel object, NULL if not found
 */
channel_t* channel_find(const char *name);

/**
 * Start all registered channels
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t channel_start_all(void);

/**
 * Stop all registered channels
 */
void channel_stop_all(void);

/**
 * Send message through channel
 * @param channel_name Channel name
 * @param session_id Session ID
 * @param content Message content
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t channel_send(const char *channel_name, const char *session_id, 
                        const char *content);

/**
 * Get number of registered channels
 * @return Number of channels
 */
int channel_get_count(void);

/**
 * Get channel by index
 * @param index Index (0-based)
 * @return Channel object, NULL if invalid index
 */
channel_t* channel_get_by_index(int index);

/**
 * Poll all channels (for handling input)
 * Non-blocking check for new messages from each channel
 */
void channel_poll_all(void);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_H */
