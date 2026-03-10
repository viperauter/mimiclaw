#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "mimi_err.h"
#include "event/event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct mimi_websocket mimi_websocket_t;

/* WebSocket event types */
typedef enum {
    MIMI_WS_EVENT_CONNECTED,
    MIMI_WS_EVENT_DISCONNECTED,
    MIMI_WS_EVENT_MESSAGE,
    MIMI_WS_EVENT_ERROR
} mimi_ws_event_t;

/* WebSocket event callback */
typedef void (*mimi_ws_event_cb)(mimi_websocket_t *ws, mimi_ws_event_t event, 
                               const uint8_t *data, size_t data_len, 
                               void *user_data);

/* WebSocket configuration */
typedef struct {
    const char *url;             /* WebSocket server URL (ws:// or wss://) */
    const char *headers;         /* Optional additional headers */
    uint32_t timeout_ms;         /* Connection timeout in milliseconds */
    uint32_t ping_interval_ms;   /* Ping interval in milliseconds */
    mimi_ws_event_cb event_cb;   /* Event callback function */
    void *user_data;             /* User data passed to callback */
} mimi_ws_config_t;

/**
 * Create WebSocket client
 * @param config WebSocket configuration
 * @return WebSocket handle on success, NULL on failure
 */
mimi_websocket_t *mimi_ws_create(const mimi_ws_config_t *config);

/**
 * Destroy WebSocket client
 * @param ws WebSocket handle
 */
void mimi_ws_destroy(mimi_websocket_t *ws);

/**
 * Connect to WebSocket server
 * @param ws WebSocket handle
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t mimi_ws_connect(mimi_websocket_t *ws);

/**
 * Disconnect from WebSocket server
 * @param ws WebSocket handle
 */
void mimi_ws_disconnect(mimi_websocket_t *ws);

/**
 * Send message over WebSocket
 * @param ws WebSocket handle
 * @param data Message data
 * @param data_len Message length
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t mimi_ws_send(mimi_websocket_t *ws, const uint8_t *data, size_t data_len);

/**
 * Check if WebSocket is connected
 * @param ws WebSocket handle
 * @return true if connected, false otherwise
 */
bool mimi_ws_is_connected(mimi_websocket_t *ws);

/**
 * Poll WebSocket for events
 * @param ws WebSocket handle
 * @param timeout_ms Poll timeout in milliseconds
 */
void mimi_ws_poll(mimi_websocket_t *ws, uint32_t timeout_ms);

/**
 * Set connection type for event messages
 * @param ws WebSocket handle
 * @param conn_type Connection type
 */
void mimi_ws_set_conn_type(mimi_websocket_t *ws, conn_type_t conn_type);

/**
 * Get connection type
 * @param ws WebSocket handle
 * @return Connection type
 */
conn_type_t mimi_ws_get_conn_type(mimi_websocket_t *ws);

/**
 * Get connection ID (mg_connection pointer as uint64_t)
 * @param ws WebSocket handle
 * @return Connection ID
 */
uint64_t mimi_ws_get_conn_id(mimi_websocket_t *ws);

#ifdef __cplusplus
}
#endif