#include "tools/tool_mcp_refresh.h"
#include "tools/providers/mcp_provider.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "log.h"

static const char *TOOL_SCHEMA =
    "{\"type\":\"object\",\"properties\":{"
      "\"max_attempts\":{\"type\":\"integer\",\"minimum\":1,\"default\":4},"
      "\"delay_ms\":{\"type\":\"integer\",\"minimum\":0,\"default\":500}"
    "},\"required\":[],\"additionalProperties\":false}";

static const char *TOOL_DESCRIPTION =
    "Trigger background MCP discovery refresh and rebuild tool registry JSON for configured MCP servers.";

const char *tool_mcp_refresh_schema_json(void) { return TOOL_SCHEMA; }
const char *tool_mcp_refresh_description(void) { return TOOL_DESCRIPTION; }

mimi_err_t tool_mcp_refresh_execute(const char *input_json,
                                      char *output, size_t output_size,
                                      const mimi_session_ctx_t *session_ctx)
{
    (void)session_ctx;

    int max_attempts = 4;
    int delay_ms = 500;

    if (input_json && input_json[0]) {
        cJSON *root = cJSON_Parse(input_json);
        if (root && cJSON_IsObject(root)) {
            cJSON *ma = cJSON_GetObjectItem(root, "max_attempts");
            cJSON *dm = cJSON_GetObjectItem(root, "delay_ms");
            if (ma) {
                if (cJSON_IsNumber(ma)) max_attempts = ma->valueint;
                else if (cJSON_IsString(ma) && ma->valuestring) max_attempts = atoi(ma->valuestring);
            }
            if (dm) {
                if (cJSON_IsNumber(dm)) delay_ms = dm->valueint;
                else if (cJSON_IsString(dm) && dm->valuestring) delay_ms = atoi(dm->valuestring);
            }
        }
        cJSON_Delete(root);
    }

    if (max_attempts < 1) max_attempts = 1;
    if (delay_ms < 0) delay_ms = 0;

    mcp_provider_request_refresh(max_attempts, delay_ms);

    if (output && output_size > 0) {
        snprintf(output, output_size,
                 "{\"status\":\"scheduled\",\"max_attempts\":%d,\"delay_ms\":%d}",
                 max_attempts, delay_ms);
    }

    return MIMI_OK;
}

