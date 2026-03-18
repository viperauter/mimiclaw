#pragma once

#include "mimi_err.h"

#include <stdint.h>

/* Subagent static config (loaded from config.json via config_view). */
typedef struct {
    char name[64];
    char type[16];
    char tools[256];
    char description[512];
    int  max_iters;
    int  timeout_sec;
    bool isolated_context;
} mimi_subagent_config_t;

typedef struct {
    mimi_subagent_config_t cfg;   /* static config from config.json */
    char system_prompt[8192];     /* loaded from SYSTEM.md (or similar) */
    char *tools_json;             /* filtered tools schema JSON (owned by this struct) */
} subagent_runtime_config_t;

/**
 * Initialize subagent runtime configs from global config + filesystem.
 * Should be called once during startup after mimi_config_load().
 */
mimi_err_t subagent_config_init(void);

/**
 * Look up a subagent runtime config by role (preferred) or name.
 * Returns NULL if not found.
 */
const subagent_runtime_config_t *subagent_get_by_role(const char *role);

