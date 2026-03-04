#pragma once

#include <stddef.h>
#include "platform/mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure a base directory for the virtual filesystem (POSIX). */
mimi_err_t mimi_fs_set_base(const char *base_dir);

/* Resolve an absolute runtime path for a configured file path (may be the same). */
mimi_err_t mimi_fs_resolve_path(const char *path_in, char *path_out, size_t path_out_len);

#ifdef __cplusplus
}
#endif

