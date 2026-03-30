/**
 * MCP Refresh Command
 *
 * Trigger background MCP discovery/refresh and rebuild tool registry JSON.
 * Usage:
 *   /mcp_refresh [max_attempts] [delay_ms]
 */

#include "interface/commands/command.h"
#include "log.h"
#include "tools/providers/mcp_provider.h"
#include "tools/tool_registry.h"
#include "cJSON.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "cmd_mcp_refresh";

static int cmd_mcp_refresh_execute(const char **args, int arg_count,
                                   const command_context_t *ctx,
                                   char *output, size_t output_len)
{
    (void)ctx;

    int max_attempts = 4;
    int delay_ms = 500;

    if (arg_count >= 1 && args[0] && args[0][0]) {
        max_attempts = atoi(args[0]);
    }
    if (arg_count >= 2 && args[1] && args[1][0]) {
        delay_ms = atoi(args[1]);
    }

    if (max_attempts < 1) max_attempts = 1;
    if (delay_ms < 0) delay_ms = 0;

    mcp_provider_request_refresh(max_attempts, delay_ms);
    snprintf(output, output_len,
             "MCP discovery refresh scheduled (max_attempts=%d delay_ms=%d). Check logs for details.",
             max_attempts, delay_ms);
    return 0;
}

static const command_t cmd_mcp_refresh = {
    .name = "mcp_refresh",
    .description = "Trigger MCP discovery refresh and rebuild tool registry JSON",
    .usage = "/mcp_refresh [max_attempts] [delay_ms]",
    .execute = cmd_mcp_refresh_execute,
};

static int cmd_mcp_status_execute(const char **args, int arg_count,
                                  const command_context_t *ctx,
                                  char *output, size_t output_len)
{
    (void)args;
    (void)arg_count;
    (void)ctx;

    const char *tools_json = tool_registry_get_tools_json();
    cJSON *arr = tools_json ? cJSON_Parse(tools_json) : NULL;
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        snprintf(output, output_len,
                 "MCP status unavailable: tools registry JSON not ready.");
        return -1;
    }

    int total = cJSON_GetArraySize(arr);
    int mcp_count = 0;
    int builtin_count = 0;

    for (int i = 0; i < total; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *name = it ? cJSON_GetObjectItemCaseSensitive(it, "name") : NULL;
        if (!cJSON_IsString(name) || !name->valuestring) continue;

        if (strncmp(name->valuestring, "mcp::", 5) == 0) {
            mcp_count++;
        } else {
            builtin_count++;
        }
    }

    snprintf(output, output_len,
             "MCP tool status:\n"
             "  total tools: %d\n"
             "  built-in tools: %d\n"
             "  third-party MCP tools: %d\n"
             "  MCP provider: %s",
             total, builtin_count, mcp_count, (mcp_count > 0) ? "ready" : "no MCP tools discovered");
    cJSON_Delete(arr);
    return 0;
}

static const command_t cmd_mcp_status = {
    .name = "mcp_status",
    .description = "Show MCP tools status (built-in vs third-party MCP)",
    .usage = "/mcp_status",
    .execute = cmd_mcp_status_execute,
};

static int append_tool_line(char *output, size_t output_len, size_t *pos,
                            const char *prefix, const char *name)
{
    if (*pos >= output_len - 1) return -1;
    int w = snprintf(output + *pos, output_len - *pos, "  - %s%s\n", prefix, name);
    if (w < 0) return -1;
    if ((size_t)w >= output_len - *pos) {
        *pos = output_len - 1;
        output[*pos] = '\0';
        return -1;
    }
    *pos += (size_t)w;
    return 0;
}

static int cmd_mcp_tools_execute(const char **args, int arg_count,
                                 const command_context_t *ctx,
                                 char *output, size_t output_len)
{
    (void)args;
    (void)arg_count;
    (void)ctx;

    const char *tools_json = tool_registry_get_tools_json();
    cJSON *arr = tools_json ? cJSON_Parse(tools_json) : NULL;
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        snprintf(output, output_len,
                 "MCP tools list unavailable: tools registry JSON not ready.");
        return -1;
    }

    int total = cJSON_GetArraySize(arr);
    size_t pos = 0;
    int truncated = 0;
    int builtin_count = 0;
    int mcp_count = 0;

    int w = snprintf(output + pos, output_len - pos,
                     "Tool list (built-in + third-party MCP)\n"
                     "[Built-in]\n");
    if (w < 0 || (size_t)w >= output_len - pos) {
        cJSON_Delete(arr);
        snprintf(output, output_len, "Output buffer too small.");
        return -1;
    }
    pos += (size_t)w;

    for (int i = 0; i < total; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *name = it ? cJSON_GetObjectItemCaseSensitive(it, "name") : NULL;
        if (!cJSON_IsString(name) || !name->valuestring) continue;
        if (strncmp(name->valuestring, "mcp::", 5) == 0) continue;
        builtin_count++;
        if (append_tool_line(output, output_len, &pos, "", name->valuestring) != 0) {
            truncated = 1;
            break;
        }
    }

    if (!truncated) {
        w = snprintf(output + pos, output_len - pos, "[Third-party MCP]\n");
        if (w < 0 || (size_t)w >= output_len - pos) {
            truncated = 1;
        } else {
            pos += (size_t)w;
        }
    }

    for (int i = 0; !truncated && i < total; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *name = it ? cJSON_GetObjectItemCaseSensitive(it, "name") : NULL;
        if (!cJSON_IsString(name) || !name->valuestring) continue;
        if (strncmp(name->valuestring, "mcp::", 5) != 0) continue;
        mcp_count++;
        if (append_tool_line(output, output_len, &pos, "", name->valuestring) != 0) {
            truncated = 1;
            break;
        }
    }

    if (!truncated && pos < output_len - 1) {
        (void)snprintf(output + pos, output_len - pos,
                       "Total: %d (built-in=%d, MCP=%d)",
                       builtin_count + mcp_count, builtin_count, mcp_count);
    } else if (output_len > 1) {
        output[output_len - 2] = '\n';
        output[output_len - 1] = '\0';
    }

    cJSON_Delete(arr);
    return 0;
}

static const command_t cmd_mcp_tools = {
    .name = "mcp_tools",
    .description = "List all tools (built-in and third-party MCP)",
    .usage = "/mcp_tools",
    .execute = cmd_mcp_tools_execute,
};

void cmd_mcp_refresh_init(void)
{
    int ret = command_register(&cmd_mcp_refresh);
    if (ret != 0) {
        MIMI_LOGE(TAG, "Failed to register /mcp_refresh command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Registered /mcp_refresh command");
    }

    ret = command_register(&cmd_mcp_status);
    if (ret != 0) {
        MIMI_LOGE(TAG, "Failed to register /mcp_status command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Registered /mcp_status command");
    }

    ret = command_register(&cmd_mcp_tools);
    if (ret != 0) {
        MIMI_LOGE(TAG, "Failed to register /mcp_tools command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Registered /mcp_tools command");
    }
}

