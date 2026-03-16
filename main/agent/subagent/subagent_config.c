#include "agent/subagent/subagent_config.h"
#include "mimi_config.h"
#include "fs/fs.h"
#include "path_utils.h"
#include "cJSON.h"
#include "config_view.h"
#include "tools/tool_registry.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "subagent_cfg";

#define MAX_SUBAGENTS_RUNTIME 8

static subagent_runtime_config_t s_subagents[MAX_SUBAGENTS_RUNTIME];
static int s_subagent_runtime_count = 0;

static size_t read_file_into_buffer(const char *path, char *buf, size_t buf_size)
{
    if (!path || !buf || buf_size == 0) return 0;
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(path, "r", &f);
    if (err != MIMI_OK) {
        MIMI_LOGW(TAG, "Failed to open subagent SYSTEM file: %s", path);
        return 0;
    }
    size_t n = 0;
    err = mimi_fs_read(f, buf, buf_size - 1, &n);
    mimi_fs_close(f);
    if (err != MIMI_OK) {
        return 0;
    }
    buf[n] = '\0';
    return n;
}

/* Build a filtered tools_json array for a subagent based on a comma-separated tool list. */
static char *build_filtered_tools_json(const char *tools_csv)
{
    const char *all_tools_json = tool_registry_get_tools_json();
    if (!all_tools_json || !tools_csv || !tools_csv[0]) {
        /* No filtering requested; return NULL so caller can fall back to global tools. */
        return NULL;
    }

    cJSON *all_arr = cJSON_Parse(all_tools_json);
    if (!all_arr || !cJSON_IsArray(all_arr)) {
        if (all_arr) cJSON_Delete(all_arr);
        return NULL;
    }

    cJSON *out_arr = cJSON_CreateArray();
    if (!out_arr) {
        cJSON_Delete(all_arr);
        return NULL;
    }

    char csv_buf[256];
    strncpy(csv_buf, tools_csv, sizeof(csv_buf) - 1);
    csv_buf[sizeof(csv_buf) - 1] = '\0';

    /* Simple CSV parsing: split by ',' and trim spaces. */
    char *saveptr = NULL;
    for (char *token = strtok_r(csv_buf, ",", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ",", &saveptr)) {
        while (*token == ' ' || *token == '\t') token++;
        size_t len = strlen(token);
        while (len > 0 && (token[len - 1] == ' ' || token[len - 1] == '\t')) {
            token[--len] = '\0';
        }
        if (len == 0) continue;

        /* Find matching tool in global tools array. */
        int n = cJSON_GetArraySize(all_arr);
        for (int i = 0; i < n; i++) {
            cJSON *tool = cJSON_GetArrayItem(all_arr, i);
            cJSON *name = cJSON_GetObjectItem(tool, "name");
            const char *tool_name = cJSON_GetStringValue(name);
            if (tool_name && strcmp(tool_name, token) == 0) {
                cJSON_AddItemToArray(out_arr, cJSON_Duplicate(tool, 1));
                break;
            }
        }
    }

    cJSON_Delete(all_arr);

    char *json_str = cJSON_PrintUnformatted(out_arr);
    cJSON_Delete(out_arr);
    return json_str;
}

static void tools_array_to_csv(const mimi_cfg_obj_t *tools_arr, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!tools_arr || !mimi_cfg_is_array(*tools_arr)) return;

    size_t off = 0;
    int n = mimi_cfg_arr_size(*tools_arr);
    for (int i = 0; i < n; i++) {
        mimi_cfg_obj_t t = mimi_cfg_arr_get(*tools_arr, i);
        const char *name = mimi_cfg_as_str(t, NULL);
        if (!name || !name[0]) continue;
        size_t name_len = strnlen(name, out_size - 1);
        if (off + name_len + 1 >= out_size) break;
        if (off > 0) out[off++] = ',';
        memcpy(out + off, name, name_len);
        off += name_len;
        out[off] = '\0';
    }
}

mimi_err_t subagent_config_init(void)
{
#if !MIMI_ENABLE_SUBAGENT
    s_subagent_runtime_count = 0;
    return MIMI_OK;
#else
    s_subagent_runtime_count = 0;
    mimi_cfg_obj_t defaults = mimi_cfg_get_obj(mimi_cfg_section("agents"), "defaults");
    /* Runtime switch: allow disabling subagents even when compiled in. */
    bool enabled = mimi_cfg_get_bool(defaults, "subagentsEnabled", true);
    if (!enabled) {
        MIMI_LOGI(TAG, "Subagents disabled by config (agents.defaults.subagentsEnabled=false)");
        return MIMI_OK;
    }
    int default_max_iters = mimi_cfg_get_int(defaults, "maxToolIterations", 40);
    if (default_max_iters <= 0) default_max_iters = 40;

    /* Prefer JSON-backed dynamic list: agents.subagents[] can be user-defined and unbounded. */
    mimi_cfg_obj_t agents = mimi_cfg_section("agents");
    mimi_cfg_obj_t arr = mimi_cfg_get_arr(agents, "subagents");
    int total = mimi_cfg_arr_size(arr);
    for (int i = 0; i < total && s_subagent_runtime_count < MAX_SUBAGENTS_RUNTIME; i++) {
        mimi_cfg_obj_t sa = mimi_cfg_arr_get(arr, i);
        if (!mimi_cfg_is_object(sa)) continue;

        subagent_runtime_config_t *dst = &s_subagents[s_subagent_runtime_count];
        memset(dst, 0, sizeof(*dst));
        memset(&dst->cfg, 0, sizeof(dst->cfg));

        /* Parse known fields (ignore unknown fields for forward compatibility). */
        const char *name = mimi_cfg_get_str(sa, "name", "");
        const char *role = mimi_cfg_get_str(sa, "role", "");
        const char *type = mimi_cfg_get_str(sa, "type", "inproc");
        const char *system_file = mimi_cfg_get_str(sa, "systemPromptFile", "");

        strncpy(dst->cfg.name, name, sizeof(dst->cfg.name) - 1);
        strncpy(dst->cfg.role, role, sizeof(dst->cfg.role) - 1);
        strncpy(dst->cfg.type, type, sizeof(dst->cfg.type) - 1);
        strncpy(dst->cfg.system_prompt_file, system_file, sizeof(dst->cfg.system_prompt_file) - 1);

        mimi_cfg_obj_t tools_arr = mimi_cfg_get_arr(sa, "tools");
        tools_array_to_csv(&tools_arr, dst->cfg.tools, sizeof(dst->cfg.tools));

        dst->cfg.max_iters = mimi_cfg_get_int(sa, "maxIters", 0);
        dst->cfg.timeout_sec = mimi_cfg_get_int(sa, "timeoutSec", 0);

        if (dst->cfg.max_iters <= 0) dst->cfg.max_iters = default_max_iters;

        /* Resolve system prompt file path relative to workspace when needed. */
        char path_buf[512];
        if (dst->cfg.system_prompt_file[0] != '\0') {
            /* If path is relative, treat it as relative to workspace. */
            if (mimi_path_is_absolute(dst->cfg.system_prompt_file)) {
                strncpy(path_buf, dst->cfg.system_prompt_file, sizeof(path_buf) - 1);
                path_buf[sizeof(path_buf) - 1] = '\0';
            } else {
                const char *workspace = mimi_cfg_get_str(defaults, "workspace", "./");
                mimi_path_join(workspace, dst->cfg.system_prompt_file,
                               path_buf, sizeof(path_buf));
            }
            (void)read_file_into_buffer(path_buf, dst->system_prompt, sizeof(dst->system_prompt));
        }

        /* Build tools_json filtered by tools CSV, if provided. */
        dst->tools_json = build_filtered_tools_json(dst->cfg.tools);

        MIMI_LOGI(TAG, "Subagent loaded: name=%s role=%s type=%s system_prompt_len=%zu",
                  dst->cfg.name, dst->cfg.role, dst->cfg.type, strlen(dst->system_prompt));
        s_subagent_runtime_count++;
    }

    return MIMI_OK;
#endif
}

const subagent_runtime_config_t *subagent_get_by_role(const char *role)
{
    if (!role || !role[0]) return NULL;
#if !MIMI_ENABLE_SUBAGENT
    (void)role;
    return NULL;
#else
    for (int i = 0; i < s_subagent_runtime_count; i++) {
        if (s_subagents[i].cfg.role[0] && strcmp(s_subagents[i].cfg.role, role) == 0) {
            return &s_subagents[i];
        }
        if (s_subagents[i].cfg.name[0] && strcmp(s_subagents[i].cfg.name, role) == 0) {
            return &s_subagents[i];
        }
    }
    return NULL;
#endif
}

