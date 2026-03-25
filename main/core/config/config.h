#pragma once

/* Public config API (JSON-backed, schema-merged).
 *
 * Runtime consumers should access configuration via `config_view.h`.
 * This header intentionally does NOT expose internal config structs.
 */

#include <stdbool.h>
#include "mimi_err.h"

/* ---- Default workspace file layout (relative to workspace root) ----
 * Keep these centralized so path structure changes are one-line edits. */
#define MIMI_DEFAULT_SOUL_FILE        "SOUL.md"
#define MIMI_DEFAULT_USER_FILE        "USER.md"
#define MIMI_DEFAULT_MEMORY_FILE      "memory/MEMORY.md"
#define MIMI_DEFAULT_SKILLS_PREFIX    "skills/"
#define MIMI_DEFAULT_SESSION_DIR      "sessions"
#define MIMI_DEFAULT_HEARTBEAT_FILE   "HEARTBEAT.md"
#define MIMI_DEFAULT_CRON_FILE        "cron.json"
#define MIMI_DEFAULT_AGENTS_FILE      "AGENTS.md"
#define MIMI_DEFAULT_TOOLS_FILE       "TOOLS.md"

#ifdef __cplusplus
extern "C" {
#endif

mimi_err_t mimi_config_load(const char *path);
mimi_err_t mimi_config_save(const char *path);

/**
 * Get the path of the currently loaded config file
 * @return Config path if loaded, NULL otherwise
 */
const char *mimi_config_get_path(void);

/**
 * Save config back to the path it was loaded from
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t mimi_config_save_current(void);

/**
 * Generic config value types
 */
typedef enum {
    MIMI_CONFIG_TYPE_STRING,
    MIMI_CONFIG_TYPE_BOOL,
    MIMI_CONFIG_TYPE_INT,
    MIMI_CONFIG_TYPE_DOUBLE
} mimi_config_type_t;

/**
 * Generic config setter - updates both JSON root (for persistence) and runtime config
 * 
 * Path format examples:
 * - "channels.wechat.bot_token"
 * - "proxy.host"
 * - "log_level"
 * 
 * @param path Dot-separated path to the config value
 * @param type Type of the value being set
 * @param value Pointer to the value
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t mimi_config_set(const char *path, mimi_config_type_t type, const void *value);

/**
 * Convenience wrapper for string values
 * @see mimi_config_set
 */
mimi_err_t mimi_config_set_string(const char *path, const char *value);

/**
 * Convenience wrapper for bool values
 * @see mimi_config_set
 */
mimi_err_t mimi_config_set_bool(const char *path, bool value);

/* Create a starter config file.
 * - Writes the merged default JSON schema (like mimi_config_save)
 * - Optionally injects default subagent definitions for out-of-box use
 *
 * Intended for first-run bootstrap only; never called automatically on existing files.
 */
mimi_err_t mimi_config_save_starter(const char *path, bool with_default_subagents);

#ifdef __cplusplus
}
#endif
