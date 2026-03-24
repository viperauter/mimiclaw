#include "tools/providers/mcp_provider.h"
#include "tools/providers/mcp_provider_internal.h"
#include "tools/providers/mcp_provider_core.h"

#include "config_view.h"
#include "tools/tool_exec.h"
#include "cJSON.h"
#include "log.h"
#include "os/os.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static const char *TAG = "mcp_provider";
#define MAX_MCP_SERVERS 8
#define MAX_MCP_TOOLS 128
static const char *MCP_PROTOCOL_VERSION = "2025-11-25";

static mcp_server_t s_servers[MAX_MCP_SERVERS];
static int s_server_count = 0;
static char *s_tools_json_merged = NULL;

static mimi_mutex_t *s_mcp_mu = NULL;
static uint64_t s_rpc_id_next = 1;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static void lock_mcp(void)
{
    if (!s_mcp_mu) return;
    (void)mimi_mutex_lock(s_mcp_mu);
}

static void unlock_mcp(void)
{
    if (!s_mcp_mu) return;
    (void)mimi_mutex_unlock(s_mcp_mu);
}

static void free_server(mcp_server_t *s)
{
    if (!s) return;
    free(s->tools_json);
    s->tools_json = NULL;
    if (!s->use_http && s->started) {
        if (s->to_child >= 0) close(s->to_child);
        if (s->from_child >= 0) close(s->from_child);
        if (s->pid > 0) {
            (void)kill(s->pid, SIGTERM);
            (void)waitpid(s->pid, NULL, 0);
        }
    }
    s->started = false;
    s->initialized = false;
    s->last_ping_ms = 0;
    s->session_id[0] = '\0';
    s->last_event_id[0] = '\0';
    s->sse_retry_ms = 1000;
    s->pid = 0;
    s->to_child = -1;
    s->from_child = -1;
}

static void clear_cache(void)
{
    free(s_tools_json_merged);
    s_tools_json_merged = NULL;
}

static mimi_err_t start_server(mcp_server_t *s)
{
    if (!s) return MIMI_ERR_INVALID_ARG;
    if (s->use_http) return MIMI_OK;
    return mcp_stdio_start(s);
}

static void handle_server_notification(mcp_server_t *s, cJSON *msg)
{
    (void)s;
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "method"));
    if (!method || !method[0]) return;
    MIMI_LOGD(TAG, "MCP notification: %s", method);
}

static void handle_server_request(mcp_server_t *s, cJSON *msg)
{
    if (!s || !msg) return;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (!id) return;

    const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "method"));
    MIMI_LOGW(TAG, "Unhandled MCP server request method=%s", method ? method : "(null)");

    cJSON *resp = cJSON_CreateObject();
    if (!resp) return;
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));

    cJSON *err = cJSON_CreateObject();
    if (!err) {
        cJSON_Delete(resp);
        return;
    }
    cJSON_AddNumberToObject(err, "code", -32601); /* Method not found */
    cJSON_AddStringToObject(err, "message", "Method not found");
    cJSON_AddStringToObject(err, "data", "Client does not support this MCP server request");
    cJSON_AddItemToObject(resp, "error", err);

    if (s->use_http) {
        char *resp_json = cJSON_PrintUnformatted(resp);
        if (resp_json) {
            (void)mcp_http_notify_post(s, resp_json);
            free(resp_json);
        }
    } else {
        (void)mcp_stdio_send_json(s, resp);
    }
    cJSON_Delete(resp);
}

static char *rpc_request(mcp_server_t *s, const char *method, cJSON *params)
{
    if (!s || !method) return NULL;

    if (s->use_http) {
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)s_rpc_id_next++);
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "jsonrpc", "2.0");
        cJSON_AddStringToObject(req, "id", id_str);
        cJSON_AddStringToObject(req, "method", method);
        if (params) cJSON_AddItemToObject(req, "params", params);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) return NULL;
        char *ret = mcp_http_exchange(s, id_str, body, handle_server_notification, handle_server_request);
        free(body);
        return ret;
    }
    lock_mcp();
    char *ret = mcp_stdio_exchange(s, &s_rpc_id_next, method, params,
                                   handle_server_notification, handle_server_request);
    unlock_mcp();
    return ret;
}

static mimi_err_t rpc_notify(mcp_server_t *s, const char *method, cJSON *params)
{
    if (!s || !method) return MIMI_ERR_INVALID_ARG;
    if (s->use_http) {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "jsonrpc", "2.0");
        cJSON_AddStringToObject(req, "method", method);
        if (params) cJSON_AddItemToObject(req, "params", params);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) return MIMI_ERR_NO_MEM;
        mimi_err_t herr = mcp_http_notify_post(s, body);
        free(body);
        return herr;
    }

    lock_mcp();
    mimi_err_t err = mcp_stdio_notify(s, method, params);
    unlock_mcp();
    return err;
}

static mimi_err_t mcp_do_initialize(mcp_server_t *s)
{
    return mcp_core_initialize(s, start_server, rpc_request, rpc_notify,
                               MCP_PROTOCOL_VERSION, "mimiclaw", "0.1.0", (double)getpid());
}

static mimi_err_t mcp_ping_if_needed(mcp_server_t *s)
{
    return mcp_core_ping_if_needed(s, now_ms, rpc_request, 30000);
}

static mimi_err_t refresh_server_tools(mcp_server_t *s)
{
    if (!s) return MIMI_ERR_INVALID_ARG;
    mimi_err_t err = mcp_do_initialize(s);
    if (err != MIMI_OK) return err;
    (void)mcp_ping_if_needed(s);
    return mcp_core_refresh_server_tools(s, mcp_do_initialize, rpc_request);
}

static mimi_err_t mcp_init(void)
{
    clear_cache();
    for (int i = 0; i < s_server_count; i++) free_server(&s_servers[i]);
    s_server_count = 0;

    if (!s_mcp_mu) {
        (void)mimi_mutex_create(&s_mcp_mu);
    }

    mimi_cfg_obj_t tools = mimi_cfg_section("tools");
    mimi_cfg_obj_t servers = mimi_cfg_get_arr(tools, "mcpServers");
    if (!mimi_cfg_is_array(servers)) {
        return MIMI_OK;
    }
    int sn = mimi_cfg_arr_size(servers);
    for (int i = 0; i < sn && s_server_count < MAX_MCP_SERVERS; i++) {
        mimi_cfg_obj_t node = mimi_cfg_arr_get(servers, i);
        if (!mimi_cfg_is_object(node)) continue;
        mcp_server_t *dst = &s_servers[s_server_count++];
        memset(dst, 0, sizeof(*dst));
        strncpy(dst->name, mimi_cfg_get_str(node, "name", ""), sizeof(dst->name) - 1);
        const char *transport = mimi_cfg_get_str(node, "transport", "");
        const char *url = mimi_cfg_get_str(node, "url", "");
        if ((transport[0] && strcmp(transport, "http") == 0) || url[0]) {
            dst->use_http = true;
        }
        strncpy(dst->command, mimi_cfg_get_str(node, "command", ""), sizeof(dst->command) - 1);
        strncpy(dst->args, mimi_cfg_get_str(node, "args", ""), sizeof(dst->args) - 1);
        strncpy(dst->url, url, sizeof(dst->url) - 1);
        dst->requires_confirmation = mimi_cfg_get_bool(node, "requires_confirmation", true);
        dst->pid = 0;
        dst->to_child = -1;
        dst->from_child = -1;
        dst->started = false;
        dst->negotiated_protocol_version[0] = '\0';
        dst->session_id[0] = '\0';
        dst->last_event_id[0] = '\0';
        dst->sse_retry_ms = 1000;
        dst->tools_json = NULL;
        if (dst->name[0] && ((dst->use_http && dst->url[0]) || (!dst->use_http && dst->command[0]))) {
            (void)refresh_server_tools(dst);
        }
    }
    MIMI_LOGI(TAG, "Configured %d MCP servers", s_server_count);
    return MIMI_OK;
}

static mimi_err_t mcp_deinit(void)
{
    clear_cache();
    for (int i = 0; i < s_server_count; i++) free_server(&s_servers[i]);
    s_server_count = 0;

    if (s_mcp_mu) {
        mimi_mutex_destroy(s_mcp_mu);
        s_mcp_mu = NULL;
    }
    return MIMI_OK;
}

static void rebuild_merged_tools_json(void)
{
    mcp_rebuild_merged_tools_json(s_servers, s_server_count, &s_tools_json_merged);
}

static const char *mcp_get_tools_json(void)
{
    for (int i = 0; i < s_server_count; i++) {
        if (!s_servers[i].tools_json || strcmp(s_servers[i].tools_json, "[]") == 0) {
            (void)refresh_server_tools(&s_servers[i]);
        }
    }
    clear_cache();
    if (!s_tools_json_merged) rebuild_merged_tools_json();
    return s_tools_json_merged ? s_tools_json_merged : "[]";
}

static mcp_server_t *find_server_by_tool(const char *tool_name, const char **out_local_tool)
{
    return mcp_find_server_by_tool(s_servers, s_server_count, tool_name, out_local_tool);
}

static mimi_err_t mcp_execute(const char *tool_name, const char *input_json,
                              char *output, size_t output_size,
                              const mimi_session_ctx_t *session_ctx)
{
    (void)session_ctx;
    const char *local_tool = NULL;
    mcp_server_t *srv = find_server_by_tool(tool_name, &local_tool);
    if (!srv) {
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"unknown mcp tool\"}");
        return MIMI_ERR_NOT_FOUND;
    }
    mimi_err_t err = mcp_do_initialize(srv);
    if (err != MIMI_OK) return err;
    (void)mcp_ping_if_needed(srv);

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", local_tool);
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    if (!args) args = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "arguments", args);
    char *resp_line = rpc_request(srv, "tools/call", params);
    if (!resp_line && srv->use_http && !srv->initialized) {
        if (mcp_do_initialize(srv) == MIMI_OK) {
            cJSON *params_retry = cJSON_CreateObject();
            cJSON_AddStringToObject(params_retry, "name", local_tool);
            cJSON *args_retry = cJSON_Parse(input_json ? input_json : "{}");
            if (!args_retry) args_retry = cJSON_CreateObject();
            cJSON_AddItemToObject(params_retry, "arguments", args_retry);
            resp_line = rpc_request(srv, "tools/call", params_retry);
        }
    }
    if (!resp_line) {
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"mcp call failed\"}");
        return MIMI_ERR_FAIL;
    }
    strncpy(output, resp_line, output_size - 1);
    output[output_size - 1] = '\0';
    free(resp_line);
    return MIMI_OK;
}

static bool mcp_requires_confirmation(const char *tool_name)
{
    const char *local_tool = NULL;
    mcp_server_t *srv = find_server_by_tool(tool_name, &local_tool);
    if (!srv) return true;
    return srv->requires_confirmation;
}

const mimi_tool_provider_t *mcp_provider_get(void)
{
    static const mimi_tool_provider_t provider = {
        .name = "mcp",
        .requires_confirmation_default = true,
        .init = mcp_init,
        .deinit = mcp_deinit,
        .get_tools_json = mcp_get_tools_json,
        .execute = mcp_execute,
        .requires_confirmation = mcp_requires_confirmation,
    };
    return &provider;
}
