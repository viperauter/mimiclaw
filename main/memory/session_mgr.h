#pragma once

#include "mimi_err.h"
#include <stddef.h>
#include <stdbool.h>

#include "bus/message_bus.h"

/**
 * Session context for tool execution. Identifies the current conversation turn
 * and provides per-session workspace root for file operations.
 */
typedef struct mimi_session_ctx {
    char channel[16];
    char chat_id[128];
    char workspace_root[256];  /* e.g. "workspaces/cli_test" - empty if no session */

    /* Subagent orchestration context.
     *
     * requester_session_key: stable owner key used for access control. For normal
     * tool calls, this is derived from channel/chat_id. For subagents, this is
     * inherited from the parent session.
     *
     * caller_session_key: identifies the immediate caller (parent or subagent).
     * For normal tool calls, equals requester_session_key.
     *
     * caller_is_subagent/subagent_id: marks tool calls originating from a
     * subagent. Used to restrict spawning / cross-session control.
     */
    char requester_session_key[192];
    char caller_session_key[192];
    bool caller_is_subagent;
    char subagent_id[64];
} mimi_session_ctx_t;

/**
 * Build session context from an inbound message. Caller provides storage.
 * workspace_root is set to "workspaces/{channel}_{chat_id}" when channel/chat_id present.
 */
void session_ctx_from_msg(const mimi_msg_t *msg, mimi_session_ctx_t *out);

/**
 * Initialize session manager.
 */
mimi_err_t session_mgr_init(void);

/**
 * Append a message to a session file (JSONL format).
 * @param channel   Channel name (e.g., "telegram", "cli", "websocket")
 * @param chat_id   Chat identifier within the channel
 * @param role      "user" or "assistant"
 * @param content   Message text
 */
mimi_err_t session_append(const char *channel, const char *chat_id, const char *role, const char *content);

/**
 * Load session history as a JSON array string suitable for LLM messages.
 * Returns the last max_msgs messages as:
 * [{"role":"user","content":"..."},{"role":"assistant","content":"..."},...]
 *
 * @param chat_id   Session identifier
 * @param buf       Output buffer (caller allocates)
 * @param size      Buffer size
 * @param max_msgs  Maximum number of messages to return
 */
mimi_err_t session_get_history_json(const char *channel, const char *chat_id, char *buf, size_t size, int max_msgs);

/**
 * Clear a session (delete the file).
 */
mimi_err_t session_clear(const char *channel, const char *chat_id);

/**
 * List all session files (prints to log).
 */
void session_list(void);
