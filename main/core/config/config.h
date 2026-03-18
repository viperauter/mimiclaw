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
