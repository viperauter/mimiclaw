#include "tools/tool_subagent.h"
#include "agent/subagent/subagent_runtime.h"
#include "mimi_config.h"
#include "cJSON.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tool_subagent";

mimi_err_t tool_subagent_run_execute(const char *input_json,
                                     char *output,
                                     size_t output_size,
                                     const mimi_session_ctx_t *session_ctx)
{
    (void)session_ctx;
    if (!output || output_size == 0) {
        return MIMI_ERR_INVALID_ARG;
    }
    output[0] = '\0';

#if !MIMI_ENABLE_TOOL_SUBAGENT
    snprintf(output, output_size, "{\"ok\":false,\"error\":\"subagent tool disabled\"}");
    return MIMI_ERR_NOT_SUPPORTED;
#endif

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid input json\"}");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(root, "role"));
    const char *task = cJSON_GetStringValue(cJSON_GetObjectItem(root, "task"));
    const char *context = cJSON_GetStringValue(cJSON_GetObjectItem(root, "context"));

    if (!role || !task) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"role and task are required\"}");
        return MIMI_ERR_INVALID_ARG;
    }

    subagent_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.task, task, sizeof(req.task) - 1);
    if (context && context[0]) {
        strncpy(req.context, context, sizeof(req.context) - 1);
    }

    subagent_result_t res;
    mimi_err_t err = subagent_run(role, &req, &res, session_ctx);
    cJSON_Delete(root);

    if (err != MIMI_OK) {
        MIMI_LOGW(TAG, "subagent_run failed: %s", mimi_err_to_name(err));
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", res.ok && err == MIMI_OK);
    cJSON_AddStringToObject(out, "role", role);
    cJSON_AddStringToObject(out, "task", req.task);
    cJSON_AddStringToObject(out, "content", res.content);
    cJSON_AddStringToObject(out, "error", res.error);

    char *json_str = cJSON_PrintUnformatted(out);
    if (!json_str) {
        cJSON_Delete(out);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"failed to serialize result\"}");
        return MIMI_ERR_NO_MEM;
    }

    strncpy(output, json_str, output_size - 1);
    output[output_size - 1] = '\0';
    free(json_str);
    cJSON_Delete(out);

    return MIMI_OK;
}

