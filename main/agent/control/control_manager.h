/**
 * Control Manager
 * 
 * The control manager is the core component of the generic control channel,
 * responsible for managing the lifecycle and state of control requests.
 * It supports various control scenarios including tool confirmation,
 * operation cancellation, stop operations, and status queries.
 */

#ifndef CONTROL_MANAGER_H
#define CONTROL_MANAGER_H

#include "mimi_err.h"
#include "bus/message_bus.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of concurrent control requests */
#define CONTROL_MAX_REQUESTS 16

/* Default timeout for control requests (30 seconds) */
#define CONTROL_DEFAULT_TIMEOUT_MS 30000

/* Request ID length */
#define CONTROL_REQUEST_ID_LEN 64

/* Control request structure */
typedef struct {
    char request_id[CONTROL_REQUEST_ID_LEN];
    char channel[16];
    char chat_id[128];
    mimi_control_type_t control_type;
    char target[64];
    void *context;                  /* Context pointer (e.g., tool_call_context_t) */
    uint64_t timeout;
    void (*callback)(const char *request_id, const char *response, void *context);
    bool active;
} control_request_t;

/**
 * Initialize the control manager
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_init(void);

/**
 * Deinitialize the control manager
 */
void control_manager_deinit(void);

/**
 * Send a control request
 * 
 * @param channel Channel name (e.g., "telegram", "cli")
 * @param chat_id Chat ID or session ID
 * @param control_type Type of control request
 * @param target Target of the control (e.g., tool name)
 * @param data Additional data for the control request
 * @param context Context pointer to be passed to callback
 * @param callback Callback function to handle the response
 * @param out_request_id Output buffer for the generated request ID
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_send_request(
    const char *channel,
    const char *chat_id,
    mimi_control_type_t control_type,
    const char *target,
    const char *data,
    void *context,
    void (*callback)(const char *request_id, const char *response, void *context),
    char *out_request_id
);

/**
 * Handle a control response
 * 
 * @param request_id Request ID from the control message
 * @param response Response string from the user
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_handle_response(const char *request_id, const char *response);

/**
 * Handle a control response by chat ID
 * Finds the most recent pending request for the given chat ID
 * 
 * @param chat_id Chat ID or session ID
 * @param response Response string from the user
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_handle_response_by_chat_id(const char *chat_id, const char *response);

/**
 * Check for timed out control requests
 * Should be called periodically (e.g., in the main loop)
 */
void control_manager_check_timeouts(void);

/**
 * Cancel a pending control request
 * 
 * @param request_id Request ID to cancel
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t control_manager_cancel_request(const char *request_id);

/**
 * Get the number of active control requests
 * 
 * @return Number of active requests
 */
int control_manager_get_active_count(void);

/**
 * Generate a unique request ID
 * 
 * @param out_id Output buffer for the request ID
 * @param id_len Length of the output buffer
 */
void control_manager_generate_request_id(char *out_id, size_t id_len);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_MANAGER_H */
