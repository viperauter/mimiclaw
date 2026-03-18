#pragma once

#include "mimi_err.h"
#include <stdbool.h>

/* Loaded from config.json under agents.subagents[]. */
typedef struct {
    char name[64];              /* human readable */
    char profile[64];           /* lookup key used by subagents.spawn.profile */
    char system_prompt_file[256];
    char tools_csv[256];        /* optional override: comma-separated allowlist (legacy-friendly) */
    int  max_iters;
    int  timeout_sec;
    bool isolated_context;
} subagent_profile_t;

typedef struct {
    subagent_profile_t cfg;
    char system_prompt[8192];   /* file contents */
    char *tools_json;           /* filtered tools schema JSON, owned (nullable) */
} subagent_profile_runtime_t;

/* Initialize profiles from global config + filesystem. */
mimi_err_t subagent_config_init(void);

/* Look up by profile key. Returns NULL if not found. */
const subagent_profile_runtime_t *subagent_profile_get(const char *profile);

/* Release tools_json allocations (optional). */
void subagent_config_deinit(void);
