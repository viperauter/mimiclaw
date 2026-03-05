#pragma once

#include "mimi_err.h"

/**
 * Initialize and start the WebSocket server on MIMI_WS_PORT.
 * Allows external clients to interact with the Agent via JSON messages.
 *
 * Protocol:
 *   Inbound:  {"type":"message","content":"hello","chat_id":"ws_client1"}
 *   Outbound: {"type":"response","content":"Hi!","chat_id":"ws_client1"}
 */
mimi_err_t ws_server_start(void);

/**
 * Send a text message to a specific WebSocket client by chat_id.
 * @param chat_id  Client identifier (assigned on connection)
 * @param text     Message text
 */
mimi_err_t ws_server_send(const char *chat_id, const char *text);

/**
 * Stop the WebSocket server.
 */
mimi_err_t ws_server_stop(void);
