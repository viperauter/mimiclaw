#include "tool_registry.h"
#include "config.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_exec.h"
#include "tools/tool_mcp_refresh.h"
#include "tools/tool_provider.h"
#include "tools/providers/mcp_provider.h"
#include "mimi_config.h"

#if MIMI_ENABLE_SUBAGENT
#include "tools/tool_subagents.h"
#include "agent/subagent/subagent_manager.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"
#include "os/os.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 12
#define MAX_TOOL_THREADS 4

typedef struct {
    char *name;
    char *input_json;
    mimi_session_ctx_t session_ctx;
    tool_callback_t callback;
    void *user_data;
} tool_task_t;

static mimi_mutex_t *s_tool_mutex = NULL;
static mimi_cond_t *s_tool_cond = NULL;
static tool_task_t s_tool_queue[16];
static int s_queue_head = 0;
static int s_queue_tail = 0;
static int s_queue_count = 0;
static bool s_worker_running = false;
static mimi_task_handle_t s_tool_workers[MAX_TOOL_THREADS] = {0};

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        MIMI_LOGE(TAG, "Tool registry full");
        return;
    }
    if (!tool->input_schema_json && tool->schema_json) {
        mimi_tool_t t = *tool;
        t.input_schema_json = t.schema_json();
        s_tools[s_tool_count++] = t;
    } else {
        s_tools[s_tool_count++] = *tool;
    }
    MIMI_LOGD(TAG, "Registered tool: %s", tool->name);
}

static const char *tool_schema_json(const mimi_tool_t *tool)
{
    if (!tool) return NULL;
    if (tool->schema_json) return tool->schema_json();
    return tool->input_schema_json;
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(tool_schema_json(&s_tools[i]));
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    const char *provider_tools = tool_provider_get_all_tools_json();
    cJSON *provider_arr = provider_tools ? cJSON_Parse(provider_tools) : NULL;
    if (provider_arr && cJSON_IsArray(provider_arr)) {
        int n = cJSON_GetArraySize(provider_arr);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(provider_arr, i);
            if (!it) continue;
            cJSON_AddItemToArray(arr, cJSON_Duplicate(it, 1));
        }
    }
    cJSON_Delete(provider_arr);

    /* Avoid freeing the old JSON buffer here: other threads/loops may still
     * hold the previous pointer. We treat refreshed JSON as a short-lived
     * generation and leak previous buffers (updated only at init/refresh). */
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    MIMI_LOGD(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

mimi_err_t tool_registry_init(void)
{
    s_tool_count = 0;
    (void)tool_provider_registry_init();
    (void)tool_provider_register(mcp_provider_get());

    /* Register web_search */
    tool_web_search_init();
    mimi_tool_t ws = {
        .name = "web_search",
        .description = tool_web_search_description(),
        .schema_json = tool_web_search_schema_json,
        .requires_confirmation = false,
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = tool_get_time_description(),
        .schema_json = tool_get_time_schema_json,
        .requires_confirmation = false,
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = tool_read_file_description(),
        .schema_json = tool_read_file_schema_json,
        .requires_confirmation = false,
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = tool_write_file_description(),
        .schema_json = tool_write_file_schema_json,
        .requires_confirmation = true,
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = tool_edit_file_description(),
        .schema_json = tool_edit_file_schema_json,
        .requires_confirmation = true,
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = tool_list_dir_description(),
        .schema_json = tool_list_dir_schema_json,
        .requires_confirmation = false,
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = tool_cron_add_description(),
        .schema_json = tool_cron_add_schema_json,
        .requires_confirmation = true,
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = tool_cron_list_description(),
        .schema_json = tool_cron_list_schema_json,
        .requires_confirmation = false,
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = tool_cron_remove_description(),
        .schema_json = tool_cron_remove_schema_json,
        .requires_confirmation = false,
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register exec (one-shot) */
    mimi_tool_t exec = {
        .name = "exec",
        .description = tool_exec_description(),
        .schema_json = tool_exec_schema_json,
        .requires_confirmation = true,
        .execute = tool_exec_execute,
    };
    register_tool(&exec);

    /* Register MCP refresh tool (background discovery + rebuild tools_json) */
    mimi_tool_t mcp_rf = {
        .name = "mcp_refresh",
        .description = tool_mcp_refresh_description(),
        .schema_json = tool_mcp_refresh_schema_json,
        .requires_confirmation = true,
        .execute = tool_mcp_refresh_execute,
    };
    register_tool(&mcp_rf);

#if MIMI_ENABLE_SUBAGENT
    /* Subagent orchestration: spawn/join/cancel/list/steer */
    (void)subagent_manager_init();
    mimi_tool_t sa = {
        .name = "subagents",
        .description = tool_subagents_description(),
        .schema_json = tool_subagents_schema_json,
        .requires_confirmation = false,
        .execute = tool_subagents_execute,
    };
    register_tool(&sa);
#endif

    build_tools_json();

    MIMI_LOGD(TAG, "Tool registry initialized");
    return MIMI_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

mimi_err_t tool_registry_refresh_tools_json(void)
{
    /* Rebuild provider tools JSON then rebuild the combined tool list JSON. */
    tool_provider_invalidate_tools_json_cache();
    build_tools_json();
    MIMI_LOGI(TAG, "Tool registry JSON refreshed");
    return MIMI_OK;
}

mimi_err_t tool_registry_execute(const char *name, const char *input_json,
                                 char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            MIMI_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size, session_ctx);
        }
    }

    MIMI_LOGW(TAG, "Unknown tool: %s", name);
    mimi_err_t perr = tool_provider_execute(name, input_json, output, output_size, session_ctx);
    if (perr != MIMI_ERR_NOT_FOUND) {
        return perr;
    }
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return MIMI_ERR_NOT_FOUND;
}

static void tool_worker_thread(void *arg)
{
    (void)arg;
    MIMI_LOGD(TAG, "Tool worker thread started");

    while (s_worker_running) {
        if (s_tool_mutex) {
            mimi_mutex_lock(s_tool_mutex);
        }

        while (s_queue_count == 0 && s_worker_running) {
            if (s_tool_cond && s_tool_mutex) {
                mimi_cond_wait(s_tool_cond, s_tool_mutex, 1000);
            } else {
                mimi_sleep_ms(100);
            }
        }

        if (!s_worker_running) {
            if (s_tool_mutex) {
                mimi_mutex_unlock(s_tool_mutex);
            }
            break;
        }

        if (s_queue_count == 0) {
            if (s_tool_mutex) {
                mimi_mutex_unlock(s_tool_mutex);
            }
            continue;
        }

        tool_task_t task = s_tool_queue[s_queue_head];
        s_queue_head = (s_queue_head + 1) % 16;
        s_queue_count--;
        if (s_tool_mutex) {
            mimi_mutex_unlock(s_tool_mutex);
        }

        char output[8192] = {0};
        mimi_err_t result = tool_registry_execute(task.name, task.input_json, output, sizeof(output), &task.session_ctx);

        if (task.callback) {
            task.callback(result, task.name, output, task.user_data);
        }
        
        if (task.input_json) {
            free((void *)task.input_json);
        }
        if (task.name) {
            free((void *)task.name);
        }
    }

    MIMI_LOGI(TAG, "Tool worker thread exiting");
    return;
}

static void start_tool_workers(void)
{
    if (s_worker_running) {
        return;
    }

    /* Initialize mutex and condition variable */
    if (s_tool_mutex == NULL) {
        mimi_err_t err = mimi_mutex_create(&s_tool_mutex);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to create mutex: %s", mimi_err_to_name(err));
            return;
        }
    }

    if (s_tool_cond == NULL) {
        mimi_err_t err = mimi_cond_create(&s_tool_cond);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to create condition: %s", mimi_err_to_name(err));
            mimi_mutex_destroy(s_tool_mutex);
            s_tool_mutex = NULL;
            return;
        }
    }

    s_worker_running = true;
    for (int i = 0; i < MAX_TOOL_THREADS; i++) {
        char thread_name[32];
        snprintf(thread_name, sizeof(thread_name), "tool_worker_%d", i);
        mimi_err_t err = mimi_task_create(thread_name, (mimi_task_fn_t)tool_worker_thread, NULL, 4096, 0, &s_tool_workers[i]);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to create tool worker thread %d: %s", i, mimi_err_to_name(err));
        }
    }
    MIMI_LOGI(TAG, "Started %d tool worker threads", MAX_TOOL_THREADS);
}

static void stop_tool_workers(void)
{
    if (!s_worker_running) {
        return;
    }

    s_worker_running = false;
    if (s_tool_cond) {
        mimi_cond_broadcast(s_tool_cond);
    }

    for (int i = 0; i < MAX_TOOL_THREADS; i++) {
        if (s_tool_workers[i]) {
            mimi_task_delete(s_tool_workers[i]);
            s_tool_workers[i] = NULL;
        }
    }

    if (s_tool_cond) {
        mimi_cond_destroy(s_tool_cond);
        s_tool_cond = NULL;
    }

    if (s_tool_mutex) {
        mimi_mutex_destroy(s_tool_mutex);
        s_tool_mutex = NULL;
    }
    MIMI_LOGI(TAG, "Stopped tool worker threads");
}

mimi_err_t tool_registry_execute_async(const char *name, const char *input_json,
                                       const mimi_session_ctx_t *session_ctx,
                                       tool_callback_t callback, void *user_data)
{
    static bool workers_started = false;
    if (!workers_started) {
        start_tool_workers();
        workers_started = true;
    }

    if (s_tool_mutex) {
        mimi_mutex_lock(s_tool_mutex);
    }

    if (s_queue_count >= 16) {
        if (s_tool_mutex) {
            mimi_mutex_unlock(s_tool_mutex);
        }
        MIMI_LOGE(TAG, "Tool queue full");
        return MIMI_ERR_NO_MEM;
    }

    // Make copies of name and input_json to avoid memory corruption
    s_tool_queue[s_queue_tail].name = name ? strdup(name) : NULL;
    s_tool_queue[s_queue_tail].input_json = input_json ? strdup(input_json) : NULL;
    MIMI_LOGI(TAG, "tool_registry_execute_async: queued task %s with input_json='%s' (copied: name=%p, input_json=%p)", 
              s_tool_queue[s_queue_tail].name, s_tool_queue[s_queue_tail].input_json, 
              s_tool_queue[s_queue_tail].name, s_tool_queue[s_queue_tail].input_json);
    if (session_ctx) {
        s_tool_queue[s_queue_tail].session_ctx = *session_ctx;
    }
    s_tool_queue[s_queue_tail].callback = callback;
    s_tool_queue[s_queue_tail].user_data = user_data;

    s_queue_tail = (s_queue_tail + 1) % 16;
    s_queue_count++;

    if (s_tool_cond) {
        mimi_cond_signal(s_tool_cond);
    }
    if (s_tool_mutex) {
        mimi_mutex_unlock(s_tool_mutex);
    }

    return MIMI_OK;
}

mimi_err_t tool_registry_execute_all_async(const tool_call_t *calls, int call_count,
                                           const mimi_session_ctx_t *session_ctx,
                                           tool_callback_t callback, void *user_data)
{
    if (!calls || call_count <= 0) {
        return MIMI_ERR_INVALID_ARG;
    }

    mimi_err_t result = MIMI_OK;
    for (int i = 0; i < call_count; i++) {
        mimi_err_t err = tool_registry_execute_async(calls[i].name, calls[i].input_json,
                                                     session_ctx, callback, user_data);
        if (err != MIMI_OK) {
            result = err;
        }
    }

    return result;
}

bool tool_registry_requires_confirmation(const char *tool_name)
{
    if (!tool_name) {
        return false;
    }
    
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, tool_name) == 0) {
            return s_tools[i].requires_confirmation;
        }
    }

    return tool_provider_requires_confirmation(tool_name, false);
}

mimi_err_t tool_registry_deinit(void)
{
    stop_tool_workers();
    
    // Cleanup other resources
    free(s_tools_json);
    s_tools_json = NULL;
    tool_provider_registry_deinit();
    
    MIMI_LOGD(TAG, "Tool registry deinitialized");
    return MIMI_OK;
}
