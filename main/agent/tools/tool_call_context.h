/**
 * Tool Call Context Manager
 *
 * Manages the lifecycle and state of tool call contexts.
 * Provides centralized management for tool execution contexts,
 * confirmation states, and asynchronous operations.
 */

#ifndef TOOL_CALL_CONTEXT_H
#define TOOL_CALL_CONTEXT_H

#include "mimi_err.h"
#include "memory/session_mgr.h"
#include "mimi_config.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Confirmation result enumeration */
typedef enum {
    CONFIRMATION_PENDING = 0,        /* Waiting for confirmation */
    CONFIRMATION_ACCEPTED,           /* User confirmed execution */
    CONFIRMATION_ACCEPTED_ALWAYS,    /* User confirmed and set always allow */
    CONFIRMATION_REJECTED,           /* User rejected execution */
    CONFIRMATION_TIMEOUT             /* Confirmation timeout */
} confirmation_result_t;

/* Tool call context structure */
typedef struct {
    /* Tool basic information */
    char tool_name[64];
    char tool_call_id[128];
    char tool_input[1024];

    /* Execution context */
    void *agent_ctx;  /* Opaque pointer to agent request context */
    mimi_session_ctx_t session_ctx;

    /* Confirmation state */
    bool requires_confirmation;
    bool waiting_for_confirmation;
    uint64_t confirmation_timeout;
    confirmation_result_t confirmation_result;

    /* Execution results */
    bool executed;
    bool succeeded;
    char output[TOOL_OUTPUT_SIZE];
    mimi_err_t error_code;

    /* Async management */
    void *async_ctx;
    void *user_data;

    /* Internal state */
    bool active;
    uint32_t ref_count;
} tool_call_context_t;

/**
 * Initialize the tool call context manager
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t tool_call_context_manager_init(void);

/**
 * Deinitialize the tool call context manager
 */
void tool_call_context_manager_deinit(void);

/**
 * Create a new tool call context
 *
 * @param agent_ctx Agent request context
 * @param tool_name Tool name
 * @param tool_call_id Tool call ID from LLM
 * @param tool_input Tool input JSON
 * @param requires_confirmation Whether this tool requires user confirmation
 * @return Pointer to created context, NULL on failure
 */
tool_call_context_t *tool_call_context_create(
    void *agent_ctx,
    const char *tool_name,
    const char *tool_call_id,
    const char *tool_input,
    bool requires_confirmation
);

/**
 * Destroy a tool call context
 * Actually decreases reference count, only frees when count reaches 0
 *
 * @param tool_ctx Tool call context to destroy
 */
void tool_call_context_destroy(tool_call_context_t *tool_ctx);

/**
 * Retain a tool call context (increase reference count)
 *
 * @param tool_ctx Tool call context
 */
void tool_call_context_retain(tool_call_context_t *tool_ctx);

/**
 * Find a pending tool context by channel and chat_id
 *
 * @param channel Channel name
 * @param chat_id Chat ID
 * @return Pointer to pending context, NULL if not found
 */
tool_call_context_t *tool_call_context_find_pending(
    const char *channel,
    const char *chat_id
);

/**
 * Find a tool context by tool call ID
 *
 * @param tool_call_id Tool call ID
 * @return Pointer to context, NULL if not found
 */
tool_call_context_t *tool_call_context_find_by_id(const char *tool_call_id);

/**
 * Check for confirmation timeouts
 * Should be called periodically
 */
void tool_call_context_check_timeouts(void);

/**
 * Get the number of pending tool contexts
 *
 * @return Number of pending contexts
 */
int tool_call_context_get_pending_count(void);

/**
 * Get the number of active tool contexts
 *
 * @return Number of active contexts
 */
int tool_call_context_get_active_count(void);

/**
 * Mark a tool as always allowed
 *
 * @param tool_name Tool name to mark
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t tool_call_context_mark_always_allowed(const char *tool_name);

/**
 * Check if a tool is always allowed
 *
 * @param tool_name Tool name to check
 * @return true if always allowed, false otherwise
 */
bool tool_call_context_is_always_allowed(const char *tool_name);

/**
 * Load always allowed tools from persistent storage
 * Should be called during initialization
 *
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t tool_call_context_load_always_allowed(void);

/**
 * Save always allowed tools to persistent storage
 *
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t tool_call_context_save_always_allowed(void);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_CALL_CONTEXT_H */
