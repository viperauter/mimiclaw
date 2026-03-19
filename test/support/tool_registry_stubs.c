#include "mimi_err.h"
#include "memory/session_mgr.h"
#include "tools/tool_provider.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_cron.h"
#include "tools/tool_exec.h"
#include "tools/providers/mcp_stdio_provider.h"
#include "os/os.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* These stubs keep tool_registry unit tests focused on routing/behavior
 * without pulling in the full runtime, providers, or OS threading layer. */

/* ---- Provider layer ---- */
mimi_err_t tool_provider_registry_init(void) { return MIMI_OK; }
void tool_provider_registry_deinit(void) {}
mimi_err_t tool_provider_register(const mimi_tool_provider_t *provider) { (void)provider; return MIMI_OK; }
const char *tool_provider_get_all_tools_json(void) { return "[]"; }
mimi_err_t tool_provider_execute(const char *tool_name, const char *input_json, char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx)
{
    (void)tool_name;
    (void)input_json;
    (void)output;
    (void)output_size;
    (void)session_ctx;
    return MIMI_ERR_NOT_FOUND;
}
bool tool_provider_requires_confirmation(const char *tool_name, bool fallback)
{
    (void)tool_name;
    return fallback;
}

/* ---- MCP stdio provider ---- */
const mimi_tool_provider_t *mcp_stdio_provider_get(void) { return NULL; }

/* ---- OS primitives used by async worker code ---- */
mimi_err_t mimi_mutex_create(mimi_mutex_t **out)
{
    if (out) *out = (mimi_mutex_t *)0x1;
    return MIMI_OK;
}
void mimi_mutex_destroy(mimi_mutex_t *m) { (void)m; }
mimi_err_t mimi_mutex_lock(mimi_mutex_t *m) { (void)m; return MIMI_OK; }
mimi_err_t mimi_mutex_unlock(mimi_mutex_t *m) { (void)m; return MIMI_OK; }
mimi_err_t mimi_cond_create(mimi_cond_t **out)
{
    if (out) *out = (mimi_cond_t *)0x1;
    return MIMI_OK;
}
void mimi_cond_destroy(mimi_cond_t *c) { (void)c; }
mimi_err_t mimi_cond_wait(mimi_cond_t *c, mimi_mutex_t *m, uint32_t timeout_ms)
{
    (void)c;
    (void)m;
    (void)timeout_ms;
    return MIMI_OK;
}
mimi_err_t mimi_cond_signal(mimi_cond_t *c) { (void)c; return MIMI_OK; }
mimi_err_t mimi_cond_broadcast(mimi_cond_t *c) { (void)c; return MIMI_OK; }
void mimi_sleep_ms(uint32_t ms) { (void)ms; }
mimi_err_t mimi_task_create(const char *name, mimi_task_fn_t fn, void *arg, uint32_t stack_size, int prio,
                            mimi_task_handle_t *out_handle)
{
    (void)name;
    (void)fn;
    (void)arg;
    (void)stack_size;
    (void)prio;
    if (out_handle) *out_handle = (mimi_task_handle_t)0x1;
    return MIMI_OK;
}
mimi_err_t mimi_task_delete(mimi_task_handle_t handle) { (void)handle; return MIMI_OK; }

/* ---- Tool stubs ---- */
mimi_err_t tool_web_search_init(void) { return MIMI_OK; }
const char *tool_web_search_description(void) { return "stub web search"; }
const char *tool_web_search_schema_json(void) { return "{}"; }
mimi_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size,
                                  const mimi_session_ctx_t *session_ctx)
{
    (void)input_json;
    (void)session_ctx;
    if (output && output_size) {
        strncpy(output, "{}", output_size - 1);
        output[output_size - 1] = '\0';
    }
    return MIMI_OK;
}

const char *tool_get_time_description(void) { return "stub get time"; }
const char *tool_get_time_schema_json(void) { return "{}"; }
mimi_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size,
                                const mimi_session_ctx_t *session_ctx)
{
    (void)input_json;
    (void)session_ctx;
    if (output && output_size) {
        strncpy(output, "\"1970-01-01T00:00:00Z\"", output_size - 1);
        output[output_size - 1] = '\0';
    }
    return MIMI_OK;
}

const char *tool_cron_add_description(void) { return "stub cron add"; }
const char *tool_cron_add_schema_json(void) { return "{}"; }
mimi_err_t tool_cron_add_execute(const char *input_json, char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx)
{
    (void)input_json;
    (void)session_ctx;
    if (output && output_size) {
        strncpy(output, "OK", output_size - 1);
        output[output_size - 1] = '\0';
    }
    return MIMI_OK;
}
const char *tool_cron_list_description(void) { return "stub cron list"; }
const char *tool_cron_list_schema_json(void) { return "{}"; }
mimi_err_t tool_cron_list_execute(const char *input_json, char *output, size_t output_size,
                                  const mimi_session_ctx_t *session_ctx)
{
    (void)input_json;
    (void)session_ctx;
    if (output && output_size) {
        strncpy(output, "[]", output_size - 1);
        output[output_size - 1] = '\0';
    }
    return MIMI_OK;
}
const char *tool_cron_remove_description(void) { return "stub cron remove"; }
const char *tool_cron_remove_schema_json(void) { return "{}"; }
mimi_err_t tool_cron_remove_execute(const char *input_json, char *output, size_t output_size,
                                    const mimi_session_ctx_t *session_ctx)
{
    (void)input_json;
    (void)session_ctx;
    if (output && output_size) {
        strncpy(output, "OK", output_size - 1);
        output[output_size - 1] = '\0';
    }
    return MIMI_OK;
}

const char *tool_exec_description(void) { return "stub exec"; }
const char *tool_exec_schema_json(void) { return "{}"; }
mimi_err_t tool_exec_execute(const char *input_json, char *output, size_t output_size,
                             const mimi_session_ctx_t *session_ctx)
{
    (void)input_json;
    (void)session_ctx;
    if (output && output_size) {
        strncpy(output, "{}", output_size - 1);
        output[output_size - 1] = '\0';
    }
    return MIMI_OK;
}

#if MIMI_ENABLE_SUBAGENT
#include "agent/subagent/subagent_manager.h"
mimi_err_t subagent_manager_init(void) { return MIMI_OK; }
#endif

