#pragma once

/*
 * Config View: ergonomic, JSON-backed, extensible config access.
 *
 * - Hides cJSON from most modules (no direct JSON manipulation required).
 * - Still supports dynamic/unknown keys for plugins (providers/tools/channels/...).
 *
 * Internals are backed by the merged raw JSON tree retained by config.c.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mimi_cfg_obj {
    const void *node; /* internal (cJSON*), opaque to callers */
} mimi_cfg_obj_t;

/* Root object of merged config. Returns NULL if config not loaded. */
mimi_cfg_obj_t mimi_cfg_root(void);

/* Section helpers (top-level object children). */
mimi_cfg_obj_t mimi_cfg_section(const char *section);                 /* e.g. "providers" */
mimi_cfg_obj_t mimi_cfg_named(const char *section, const char *name); /* e.g. providers["openrouter"] */

/* Object navigation. */
mimi_cfg_obj_t mimi_cfg_get_obj(mimi_cfg_obj_t parent, const char *key);
mimi_cfg_obj_t mimi_cfg_get_arr(mimi_cfg_obj_t parent, const char *key);

/* Array helpers. */
int         mimi_cfg_arr_size(mimi_cfg_obj_t arr);
mimi_cfg_obj_t mimi_cfg_arr_get(mimi_cfg_obj_t arr, int idx);

/* Scalar getters on object key. */
const char *mimi_cfg_get_str(mimi_cfg_obj_t obj, const char *key, const char *fallback);
bool        mimi_cfg_get_bool(mimi_cfg_obj_t obj, const char *key, bool fallback);
int         mimi_cfg_get_int(mimi_cfg_obj_t obj, const char *key, int fallback);
double      mimi_cfg_get_double(mimi_cfg_obj_t obj, const char *key, double fallback);

/* Direct scalar getters on current node (for array elements). */
const char *mimi_cfg_as_str(mimi_cfg_obj_t node, const char *fallback);
bool        mimi_cfg_as_bool(mimi_cfg_obj_t node, bool fallback);
int         mimi_cfg_as_int(mimi_cfg_obj_t node, int fallback);
double      mimi_cfg_as_double(mimi_cfg_obj_t node, double fallback);

/* Introspection. */
bool mimi_cfg_is_object(mimi_cfg_obj_t node);
bool mimi_cfg_is_array(mimi_cfg_obj_t node);

/* Iterate object keys (for plugins). */
typedef void (*mimi_cfg_each_key_cb)(void *ctx, const char *key, mimi_cfg_obj_t value);
void mimi_cfg_each_key(mimi_cfg_obj_t obj, mimi_cfg_each_key_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

