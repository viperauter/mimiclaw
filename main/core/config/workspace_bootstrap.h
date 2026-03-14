#pragma once

#include "mimi_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize FS base (Scheme B) and ensure the on-disk workspace has the
 * expected directory/file layout.
 *
 * This is primarily for POSIX builds (where the VFS root maps to a real directory).
 * It is safe to call multiple times; it never overwrites existing files.
 *
 * Precondition:
 * - mimi_config_load() has been called (so defaults/paths exist)
 *
 * If create_starter_config_if_missing is true and config_path does not exist,
 * this writes a starter JSON config file at config_path (best-effort).
 *
 * Scheme B is applied: base = "<cfg->workspace>/workspace".
 */
mimi_err_t mimi_workspace_bootstrap(const char *config_path,
                                    bool create_starter_config_if_missing);

#ifdef __cplusplus
}
#endif

