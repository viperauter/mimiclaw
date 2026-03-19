#include "tools/providers/mcp_stdio_provider.h"

#include "config_view.h"
#include "tools/tool_exec.h"
#include "cJSON.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static const char *TAG = "mcp_stdio_provider";
#define MAX_MCP_SERVERS 8
#define MAX_MCP_TOOLS 128

typedef struct {
    char name[64];
    char command[256];
    bool requires_confirmation;

    pid_t pid;
    int to_child;   /* parent writes -> child stdin */
    int from_child; /* parent reads  <- child stdout */
    bool started;

    /* cached tool list (OpenAI tools json array string) */
    char *tools_json;
} mcp_server_t;

static mcp_server_t s_servers[MAX_MCP_SERVERS];
static int s_server_count = 0;
static char *s_tools_json_merged = NULL;

static void free_server(mcp_server_t *s)
{
    if (!s) return;
    free(s->tools_json);
    s->tools_json = NULL;
    if (s->started) {
        if (s->to_child >= 0) close(s->to_child);
        if (s->from_child >= 0) close(s->from_child);
        if (s->pid > 0) {
            (void)kill(s->pid, SIGTERM);
            (void)waitpid(s->pid, NULL, 0);
        }
    }
    s->started = false;
    s->pid = 0;
    s->to_child = -1;
    s->from_child = -1;
}

static void clear_cache(void)
{
    free(s_tools_json_merged);
    s_tools_json_merged = NULL;
}

static int split_cmd(const char *cmd, char **argv, int argv_cap, char *storage, size_t storage_cap)
{
    if (!cmd || !argv || argv_cap < 2 || !storage || storage_cap == 0) return 0;
    strncpy(storage, cmd, storage_cap - 1);
    storage[storage_cap - 1] = '\0';
    int argc = 0;
    char *p = storage;
    while (*p && argc < argv_cap - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static mimi_err_t start_server(mcp_server_t *s)
{
    if (!s || !s->command[0]) return MIMI_ERR_INVALID_ARG;
    if (s->started) return MIMI_OK;

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) < 0) return MIMI_ERR_FAIL;
    if (pipe(out_pipe) < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        return MIMI_ERR_FAIL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return MIMI_ERR_FAIL;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        char storage[512];
        char *argv[32];
        int argc = split_cmd(s->command, argv, 32, storage, sizeof(storage));
        if (argc <= 0) _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    s->pid = pid;
    s->to_child = in_pipe[1];
    s->from_child = out_pipe[0];
    s->started = true;
    MIMI_LOGI(TAG, "Started MCP server %s pid=%d", s->name, (int)pid);
    return MIMI_OK;
}

static mimi_err_t rpc_send_line(mcp_server_t *s, const char *line)
{
    if (!s || !s->started || s->to_child < 0 || !line) return MIMI_ERR_INVALID_ARG;
    size_t n = strlen(line);
    if (write(s->to_child, line, n) != (ssize_t)n) return MIMI_ERR_FAIL;
    if (write(s->to_child, "\n", 1) != 1) return MIMI_ERR_FAIL;
    return MIMI_OK;
}

static mimi_err_t rpc_read_line(mcp_server_t *s, char *buf, size_t buf_sz)
{
    if (!s || !s->started || s->from_child < 0 || !buf || buf_sz == 0) return MIMI_ERR_INVALID_ARG;
    size_t off = 0;
    while (off + 1 < buf_sz) {
        char c;
        ssize_t r = read(s->from_child, &c, 1);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            return MIMI_ERR_FAIL;
        }
        if (c == '\n') break;
        buf[off++] = c;
    }
    buf[off] = '\0';
    return (off > 0) ? MIMI_OK : MIMI_ERR_FAIL;
}

static char *rpc_request(mcp_server_t *s, const char *method, cJSON *params)
{
    if (!s || !method) return NULL;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "id", "1");
    cJSON_AddStringToObject(req, "method", method);
    if (params) cJSON_AddItemToObject(req, "params", params);
    char *line = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!line) return NULL;
    if (rpc_send_line(s, line) != MIMI_OK) {
        free(line);
        return NULL;
    }
    free(line);

    char resp_line[16384];
    if (rpc_read_line(s, resp_line, sizeof(resp_line)) != MIMI_OK) return NULL;
    return strdup(resp_line);
}

static char *mcp_tools_to_openai_json(const char *server_name, cJSON *tools_arr)
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

static mimi_err_t refresh_server_tools(mcp_server_t *s)
{
    if (!s) return MIMI_ERR_INVALID_ARG;
    mimi_err_t err = start_server(s);
    if (err != MIMI_OK) return err;

    /* initialize (best-effort) */
    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    cJSON *cap = cJSON_CreateObject();
    cJSON_AddItemToObject(init_params, "capabilities", cap);
    cJSON_AddStringToObject(init_params, "clientInfo", "mimiclaw");
    char *init_resp = rpc_request(s, "initialize", init_params);
    free(init_resp);

    char *tools_resp = rpc_request(s, "tools/list", cJSON_CreateObject());
    if (!tools_resp) return MIMI_ERR_FAIL;
    cJSON *root = cJSON_Parse(tools_resp);
    free(tools_resp);
    if (!root) return MIMI_ERR_FAIL;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *tools = result ? cJSON_GetObjectItem(result, "tools") : NULL;
    char *openai = mcp_tools_to_openai_json(s->name, tools);
    free(s->tools_json);
    s->tools_json = openai;
    cJSON_Delete(root);
    return MIMI_OK;
}

static mimi_err_t mcp_init(void)
{
    clear_cache();
    for (int i = 0; i < s_server_count; i++) free_server(&s_servers[i]);
    s_server_count = 0;

    mimi_cfg_obj_t providers = mimi_cfg_section("providers");
    mimi_cfg_obj_t servers = mimi_cfg_get_arr(providers, "mcpServers");
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
        strncpy(dst->command, mimi_cfg_get_str(node, "command", ""), sizeof(dst->command) - 1);
        dst->requires_confirmation = mimi_cfg_get_bool(node, "requires_confirmation", true);
        dst->pid = 0;
        dst->to_child = -1;
        dst->from_child = -1;
        dst->started = false;
        dst->tools_json = NULL;
        if (dst->name[0] && dst->command[0]) {
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
    return MIMI_OK;
}

static void rebuild_merged_tools_json(void)
{
    clear_cache();
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;
    for (int i = 0; i < s_server_count; i++) {
        if (!s_servers[i].tools_json) continue;
        cJSON *server_arr = cJSON_Parse(s_servers[i].tools_json);
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
    s_tools_json_merged = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
}

static const char *mcp_get_tools_json(void)
{
    if (!s_tools_json_merged) rebuild_merged_tools_json();
    return s_tools_json_merged ? s_tools_json_merged : "[]";
}

static mcp_server_t *find_server_by_tool(const char *tool_name, const char **out_local_tool)
{
    if (out_local_tool) *out_local_tool = NULL;
    if (!tool_name) return NULL;
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
    for (int i = 0; i < s_server_count; i++) {
        if (strcmp(s_servers[i].name, server) == 0) return &s_servers[i];
    }
    return NULL;
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
    mimi_err_t err = start_server(srv);
    if (err != MIMI_OK) return err;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", local_tool);
    cJSON *args = cJSON_Parse(input_json ? input_json : "{}");
    if (!args) args = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "arguments", args);
    char *resp_line = rpc_request(srv, "tools/call", params);
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

const mimi_tool_provider_t *mcp_stdio_provider_get(void)
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
