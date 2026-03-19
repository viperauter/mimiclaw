#pragma once

#include "mimi_err.h"
#include <stdbool.h>

/**
 * Subagent profile configuration.
 *
 * Profiles are loaded from `config.json` under `agents.subagents[]` and provide
 * the runtime defaults for subagent execution:
 * - profile key / display name
 * - system prompt file (loaded into memory)
 * - tool allowlist (compiled into a filtered tools schema)
 * - iteration/time limits
 *
 * Minimal-config UX: callers may omit name/profile/systemPromptFile in JSON; the loader
 * will default to profile="default" and systemPromptFile="agents/default/SYSTEM.md".
 */
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

/**
 * Initialize and load profiles from global config + filesystem.
 *
 * Respects runtime enable switch `agents.defaults.subagentsEnabled`. When disabled,
 * no profiles are loaded and spawning will fail.
 */
mimi_err_t subagent_config_init(void);

/**
 * Look up a profile by key.
 *
 * @param profile Profile lookup key (e.g. "default").
 * @return Pointer to runtime profile on success, NULL if not found.
 */
const subagent_profile_runtime_t *subagent_profile_get(const char *profile);

/** Release any owned allocations (e.g. filtered tools_json). */
void subagent_config_deinit(void);
