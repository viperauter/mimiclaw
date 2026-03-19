#include "tools/tool_provider.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"
#include "log.h"

static const char *TAG = "tool_provider";
#define MAX_PROVIDERS 8

static mimi_tool_provider_t s_providers[MAX_PROVIDERS];
static int s_provider_count = 0;
static char *s_tools_json_cache = NULL;

static void invalidate_cache(void)
{
    free(s_tools_json_cache);
    s_tools_json_cache = NULL;
}

mimi_err_t tool_provider_registry_init(void)
{
    s_provider_count = 0;
    invalidate_cache();
    return MIMI_OK;
}

void tool_provider_registry_deinit(void)
{
    for (int i = 0; i < s_provider_count; i++) {
        if (s_providers[i].deinit) {
            (void)s_providers[i].deinit();
        }
    }
    s_provider_count = 0;
    invalidate_cache();
}

mimi_err_t tool_provider_register(const mimi_tool_provider_t *provider)
{
    if (!provider || !provider->name || !provider->name[0]) {
        return MIMI_ERR_INVALID_ARG;
    }
    if (s_provider_count >= MAX_PROVIDERS) {
        return MIMI_ERR_NO_MEM;
    }
    if (provider->init) {
        mimi_err_t err = provider->init();
        if (err != MIMI_OK) {
            return err;
        }
    }
    s_providers[s_provider_count++] = *provider;
    invalidate_cache();
    MIMI_LOGI(TAG, "Registered provider: %s", provider->name);
    return MIMI_OK;
}

const char *tool_provider_get_all_tools_json(void)
{
    if (s_tools_json_cache) {
        return s_tools_json_cache;
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return "[]";

    for (int i = 0; i < s_provider_count; i++) {
        if (!s_providers[i].get_tools_json) continue;
        const char *tools = s_providers[i].get_tools_json();
        if (!tools || !tools[0]) continue;
        cJSON *provider_arr = cJSON_Parse(tools);
        if (!provider_arr || !cJSON_IsArray(provider_arr)) {
            cJSON_Delete(provider_arr);
            continue;
        }
        int n = cJSON_GetArraySize(provider_arr);
        for (int j = 0; j < n; j++) {
            cJSON *it = cJSON_GetArrayItem(provider_arr, j);
            if (!it) continue;
            cJSON_AddItemToArray(arr, cJSON_Duplicate(it, 1));
        }
        cJSON_Delete(provider_arr);
    }

    s_tools_json_cache = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s_tools_json_cache) {
        s_tools_json_cache = strdup("[]");
    }
    return s_tools_json_cache ? s_tools_json_cache : "[]";
}

static bool has_prefix(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

mimi_err_t tool_provider_execute(const char *tool_name, const char *input_json,
                                 char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx)
{
    if (!tool_name) return MIMI_ERR_INVALID_ARG;
    for (int i = 0; i < s_provider_count; i++) {
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "%s::", s_providers[i].name);
        if (!has_prefix(tool_name, prefix)) continue;
        if (!s_providers[i].execute) return MIMI_ERR_NOT_SUPPORTED;
        return s_providers[i].execute(tool_name, input_json, output, output_size, session_ctx);
    }
    return MIMI_ERR_NOT_FOUND;
}

bool tool_provider_requires_confirmation(const char *tool_name, bool fallback)
{
    if (!tool_name) return fallback;
    for (int i = 0; i < s_provider_count; i++) {
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "%s::", s_providers[i].name);
        if (!has_prefix(tool_name, prefix)) continue;
        if (s_providers[i].requires_confirmation) {
            return s_providers[i].requires_confirmation(tool_name);
        }
        return s_providers[i].requires_confirmation_default;
    }
    return fallback;
}
