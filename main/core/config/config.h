#pragma once

/* Public config API (JSON-backed, schema-merged).
 *
 * Runtime consumers should access configuration via `config_view.h`.
 * This header intentionally does NOT expose internal config structs.
 */

#include <stdbool.h>
#include "mimi_err.h"

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
