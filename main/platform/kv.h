#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "platform/mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* KV backend init; POSIX implementation persists to a JSON file path. */
mimi_err_t mimi_kv_init(const char *persist_path);

mimi_err_t mimi_kv_get_str(const char *ns, const char *key, char *out, size_t out_len, bool *found);
mimi_err_t mimi_kv_set_str(const char *ns, const char *key, const char *value);

mimi_err_t mimi_kv_get_u32(const char *ns, const char *key, uint32_t *out, bool *found);
mimi_err_t mimi_kv_set_u32(const char *ns, const char *key, uint32_t value);

#ifdef __cplusplus
}
#endif

