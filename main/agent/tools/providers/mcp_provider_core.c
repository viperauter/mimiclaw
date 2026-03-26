#include "tools/providers/mcp_provider_core.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

char *mcp_tools_to_openai_json(const char *server_name, cJSON *tools_arr)
{
    if (!server_name || !tools_arr || !cJSON_IsArray(tools_arr)) return strdup("[]");
    cJSON *out = cJSON_CreateArray();
    int n = cJSON_GetArraySize(tools_arr);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(tools_arr, i);
        if (!it || !cJSON_IsObject(it)) continue;
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(it, "description"));
        cJSON *input_schema = cJSON_GetObjectItem(it, "inputSchema");
        if (!name || !name[0]) continue;
        cJSON *tool = cJSON_CreateObject();
        char fq[192];
        snprintf(fq, sizeof(fq), "mcp::%s::%s", server_name, name);
        cJSON_AddStringToObject(tool, "name", fq);
        cJSON_AddStringToObject(tool, "description", desc ? desc : "MCP tool");
        if (input_schema) cJSON_AddItemToObject(tool, "input_schema", cJSON_Duplicate(input_schema, 1));
        else cJSON_AddItemToObject(tool, "input_schema", cJSON_Parse("{\"type\":\"object\",\"properties\":{},\"required\":[]}"));
        cJSON_AddItemToArray(out, tool);
    }
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s ? s : strdup("[]");
}

mcp_server_t *mcp_find_server_by_tool(mcp_server_t *servers, int server_count,
                                      const char *tool_name, const char **out_local_tool)
{
    if (out_local_tool) *out_local_tool = NULL;
    if (!servers || server_count <= 0 || !tool_name) return NULL;
    const char *p = strstr(tool_name, "mcp::");
    if (p != tool_name) return NULL;
    p += 5;
    const char *sep = strstr(p, "::");
    if (!sep) return NULL;
    size_t server_len = (size_t)(sep - p);
    char server[64];
    if (server_len == 0 || server_len >= sizeof(server)) return NULL;
    memcpy(server, p, server_len);
    server[server_len] = '\0';
    const char *local_tool = sep + 2;
    if (!local_tool[0]) return NULL;
    if (out_local_tool) *out_local_tool = local_tool;
    for (int i = 0; i < server_count; i++) {
        if (strcmp(servers[i].name, server) == 0) return &servers[i];
    }
    return NULL;
}

void mcp_rebuild_merged_tools_json(mcp_server_t *servers, int server_count, char **merged_cache)
{
    if (!merged_cache) return;
    free(*merged_cache);
    *merged_cache = NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;
    for (int i = 0; i < server_count; i++) {
        if (!servers[i].tools_json) continue;
        cJSON *server_arr = cJSON_Parse(servers[i].tools_json);
        if (!server_arr || !cJSON_IsArray(server_arr)) {
            cJSON_Delete(server_arr);
            continue;
        }
        int n = cJSON_GetArraySize(server_arr);
        for (int j = 0; j < n; j++) {
            cJSON *it = cJSON_GetArrayItem(server_arr, j);
            if (!it) continue;
            cJSON_AddItemToArray(arr, cJSON_Duplicate(it, 1));
        }
        cJSON_Delete(server_arr);
    }
    *merged_cache = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
}

mimi_err_t mcp_core_initialize(mcp_server_t *s,
                               mcp_start_fn_t start_fn,
                               mcp_rpc_request_fn_t request_fn,
                               mcp_rpc_notify_fn_t notify_fn,
                               const char *protocol_version,
                               const char *client_name,
                               const char *client_version,
                               double process_id)
{
    if (!s || !start_fn || !request_fn || !notify_fn || !protocol_version || !client_name || !client_version) {
        return MIMI_ERR_INVALID_ARG;
    }
    if (s->initialized) return MIMI_OK;

    /* Preserve configured HTTP mode for explicitly specified transport types:
     * - MCP_TRANSPORT_SSE = force legacy SSE mode
     * - MCP_TRANSPORT_STREAMABLE_HTTP = force streamable mode
     * Only reset to UNKNOWN for auto-detection mode.
     */
    if (s->transport_type == MCP_TRANSPORT_SSE) {
        s->http_mode = MCP_HTTP_MODE_LEGACY_HTTP_SSE;
    } else if (s->transport_type == MCP_TRANSPORT_STREAMABLE_HTTP) {
        s->http_mode = MCP_HTTP_MODE_STREAMABLE;
    } else {
        s->http_mode = MCP_HTTP_MODE_UNKNOWN;
    }
    s->session_id[0] = '\0';
    s->last_event_id[0] = '\0';
    s->sse_message_url[0] = '\0';
    s->sse_retry_ms = 1000;

    mimi_err_t err = start_fn(s);
    if (err != MIMI_OK) return err;

    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", protocol_version);
    cJSON *cap = cJSON_CreateObject();
    cJSON_AddItemToObject(init_params, "capabilities", cap);
    cJSON *client_info = cJSON_CreateObject();
    cJSON_AddStringToObject(client_info, "name", client_name);
    cJSON_AddStringToObject(client_info, "version", client_version);
    cJSON_AddItemToObject(init_params, "clientInfo", client_info);
    cJSON_AddNumberToObject(init_params, "processId", process_id);

    char *init_resp = request_fn(s, "initialize", init_params);
    bool init_ok = (init_resp != NULL);
    if (init_resp) {
        cJSON *root = cJSON_Parse(init_resp);
        if (root) {
            cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
            if (e) init_ok = false;
            cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
            if (result && cJSON_IsObject(result)) {
                const char *negotiated = cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(result, "protocolVersion"));
                if (negotiated && negotiated[0]) {
                    strncpy(s->negotiated_protocol_version, negotiated,
                            sizeof(s->negotiated_protocol_version) - 1);
                    s->negotiated_protocol_version[sizeof(s->negotiated_protocol_version) - 1] = '\0';
                }
            }
            cJSON_Delete(root);
        }
        free(init_resp);
    }

    if (!init_ok) return MIMI_ERR_FAIL;
    (void)notify_fn(s, "notifications/initialized", NULL);
    s->initialized = true;
    s->last_ping_ms = 0;
    if (!s->negotiated_protocol_version[0]) {
        strncpy(s->negotiated_protocol_version, protocol_version,
                sizeof(s->negotiated_protocol_version) - 1);
        s->negotiated_protocol_version[sizeof(s->negotiated_protocol_version) - 1] = '\0';
    }
    return MIMI_OK;
}

mimi_err_t mcp_core_ping_if_needed(mcp_server_t *s,
                                   mcp_now_ms_fn_t now_fn,
                                   mcp_rpc_request_fn_t request_fn,
                                   long long interval_ms)
{
    if (!s || !now_fn || !request_fn) return MIMI_ERR_INVALID_ARG;
    if (!s->initialized) return MIMI_OK;
    long long n = now_fn();
    if (s->last_ping_ms <= 0 || n - s->last_ping_ms > interval_ms) {
        char *resp = request_fn(s, "ping", NULL);
        free(resp);
        s->last_ping_ms = n;
    }
    return MIMI_OK;
}

mimi_err_t mcp_core_refresh_server_tools(mcp_server_t *s,
                                         mcp_init_fn_t init_fn,
                                         mcp_rpc_request_fn_t request_fn)
{
    if (!s || !init_fn || !request_fn) return MIMI_ERR_INVALID_ARG;
    mimi_err_t err = init_fn(s);
    if (err != MIMI_OK) return err;

    char *tools_resp = request_fn(s, "tools/list", cJSON_CreateObject());
    if (!tools_resp && s->use_http && !s->initialized) {
        if (init_fn(s) == MIMI_OK) {
            tools_resp = request_fn(s, "tools/list", cJSON_CreateObject());
        }
    }
    if (!tools_resp) return MIMI_ERR_FAIL;

    cJSON *root = cJSON_Parse(tools_resp);
    free(tools_resp);
    if (!root) return MIMI_ERR_FAIL;

    cJSON *tools = NULL;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (result && cJSON_IsObject(result)) {
        tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    }
    if (!tools) tools = cJSON_GetObjectItemCaseSensitive(root, "tools");

    char *openai = mcp_tools_to_openai_json(s->name, tools);
    free(s->tools_json);
    s->tools_json = openai;
    cJSON_Delete(root);
    return MIMI_OK;
}
