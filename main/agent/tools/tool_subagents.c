#include "tools/tool_subagents.h"
#include "agent/subagent/subagent_manager.h"
#include "cJSON.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "tool_subagents";

static const char *SCHEMA =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"spawn\",\"join\",\"cancel\",\"list\",\"steer\"],\"default\":\"list\"},"
      "\"id\":{\"type\":\"string\",\"description\":\"Subagent id (for join/cancel/steer)\"},"
      "\"mode\":{\"type\":\"string\",\"enum\":[\"cancel\",\"kill\"],\"default\":\"cancel\"},"
      "\"waitMs\":{\"type\":\"integer\",\"minimum\":0,\"default\":0},"
      "\"recentMinutes\":{\"type\":\"integer\",\"minimum\":1},"
      "\"message\":{\"type\":\"string\"},"
      "\"task\":{\"type\":\"string\"},"
      "\"context\":{\"type\":\"string\"},"
      "\"maxIters\":{\"type\":\"integer\",\"minimum\":1},"
      "\"timeoutSec\":{\"type\":\"integer\",\"minimum\":1},"
      "\"tools\":{\"type\":\"string\",\"description\":\"Optional allowlist override as CSV. Use 'exec' for one-shot commands and 'process' for session control.\"}"
    "},"
    "\"required\":[],"
    "\"additionalProperties\":false"
    "}";

const char *tool_subagents_schema_json(void) { return SCHEMA; }
const char *tool_subagents_description(void)
{
    return "Spawn, list, steer, join, or cancel in-proc subagents for this requester session.";
}

static void write_json(char *output, size_t output_size, cJSON *obj, mimi_err_t fallback_err)
{
    if (!output || output_size == 0) return;
    output[0] = '\0';
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) {
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"%s\"}", mimi_err_to_name(fallback_err));
        return;
    }
    strncpy(output, s, output_size - 1);
    output[output_size - 1] = '\0';
    free(s);
}

mimi_err_t tool_subagents_execute(const char *input_json,
                                 char *output,
                                 size_t output_size,
                                 const mimi_session_ctx_t *session_ctx)
{
    if (!output || output_size == 0 || !session_ctx) return MIMI_ERR_INVALID_ARG;
    output[0] = '\0';

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"invalid input json\"}");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    if (!action || !action[0]) action = "list";

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "action", action);
    cJSON_AddStringToObject(resp, "requesterSessionKey", session_ctx->requester_session_key);

    mimi_err_t err = MIMI_OK;

    if (strcmp(action, "spawn") == 0) {
        const char *profile = cJSON_GetStringValue(cJSON_GetObjectItem(root, "profile"));
        const char *task = cJSON_GetStringValue(cJSON_GetObjectItem(root, "task"));
        const char *context = cJSON_GetStringValue(cJSON_GetObjectItem(root, "context"));
        const char *tools = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tools"));
        int maxIters = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "maxIters"));
        int timeoutSec = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "timeoutSec"));

        if (!task || !task[0]) {
            cJSON_AddStringToObject(resp, "status", "error");
            cJSON_AddStringToObject(resp, "error", "task is required");
            write_json(output, output_size, resp, MIMI_ERR_INVALID_ARG);
            cJSON_Delete(resp);
            cJSON_Delete(root);
            return MIMI_ERR_INVALID_ARG;
        }
        if (!profile || !profile[0]) profile = "default";

        subagent_spawn_spec_t spec;
        memset(&spec, 0, sizeof(spec));
        strncpy(spec.profile, profile, sizeof(spec.profile) - 1);
        strncpy(spec.task, task, sizeof(spec.task) - 1);
        if (context) strncpy(spec.context, context, sizeof(spec.context) - 1);
        if (tools) strncpy(spec.tools_csv, tools, sizeof(spec.tools_csv) - 1);
        spec.max_iters = maxIters;
        spec.timeout_sec = timeoutSec;
        spec.isolated_context = true;

        char id[64];
        err = subagent_spawn(&spec, id, sizeof(id), session_ctx);
        if (err == MIMI_OK) {
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "id", id);
        } else {
            cJSON_AddStringToObject(resp, "status", (err == MIMI_ERR_PERMISSION_DENIED) ? "forbidden" : "error");
            cJSON_AddStringToObject(resp, "error", mimi_err_to_name(err));
        }
    } else if (strcmp(action, "join") == 0) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
        int waitMs = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "waitMs"));
        if (!id || !id[0]) {
            cJSON_AddStringToObject(resp, "status", "error");
            cJSON_AddStringToObject(resp, "error", "id is required");
            write_json(output, output_size, resp, MIMI_ERR_INVALID_ARG);
            cJSON_Delete(resp);
            cJSON_Delete(root);
            return MIMI_ERR_INVALID_ARG;
        }
        subagent_join_result_t jr;
        memset(&jr, 0, sizeof(jr));
        err = subagent_join(id, waitMs, &jr, session_ctx);
        if (err == MIMI_OK) {
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "id", id);
            cJSON_AddBoolToObject(resp, "finished", jr.finished);
            cJSON_AddStringToObject(resp, "reason", subagent_reason_name(jr.reason));
            cJSON_AddBoolToObject(resp, "ok", jr.ok);
            cJSON_AddBoolToObject(resp, "truncated", jr.truncated);
            cJSON_AddStringToObject(resp, "content", jr.content);
            cJSON_AddStringToObject(resp, "final", jr.final_text);
            cJSON_AddStringToObject(resp, "error", jr.error);
        } else {
            cJSON_AddStringToObject(resp, "status", (err == MIMI_ERR_PERMISSION_DENIED) ? "forbidden" : "error");
            cJSON_AddStringToObject(resp, "error", mimi_err_to_name(err));
        }
    } else if (strcmp(action, "cancel") == 0) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
        const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mode"));
        if (!id || !id[0]) id = "all";
        subagent_cancel_mode_t m = (mode && strcmp(mode, "kill") == 0) ? SUBAGENT_CANCEL_KILL : SUBAGENT_CANCEL_SOFT;
        int count = 0;
        err = subagent_cancel(id, m, &count, session_ctx);
        if (err == MIMI_OK) {
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "id", id);
            cJSON_AddNumberToObject(resp, "count", count);
        } else {
            cJSON_AddStringToObject(resp, "status", (err == MIMI_ERR_PERMISSION_DENIED) ? "forbidden" : "error");
            cJSON_AddStringToObject(resp, "error", mimi_err_to_name(err));
        }
    } else if (strcmp(action, "steer") == 0) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
        const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
        if (!id || !id[0] || !message || !message[0]) {
            cJSON_AddStringToObject(resp, "status", "error");
            cJSON_AddStringToObject(resp, "error", "id and message are required");
            write_json(output, output_size, resp, MIMI_ERR_INVALID_ARG);
            cJSON_Delete(resp);
            cJSON_Delete(root);
            return MIMI_ERR_INVALID_ARG;
        }
        int depth = 0;
        err = subagent_steer(id, message, &depth, session_ctx);
        if (err == MIMI_OK) {
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "id", id);
            cJSON_AddNumberToObject(resp, "queueDepth", depth);
        } else {
            cJSON_AddStringToObject(resp, "status", (err == MIMI_ERR_PERMISSION_DENIED) ? "forbidden" : "error");
            cJSON_AddStringToObject(resp, "error", mimi_err_to_name(err));
        }
    } else {
        /* list */
        int recent = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "recentMinutes"));
        char buf[8192];
        err = subagent_list(recent, buf, sizeof(buf), session_ctx);
        if (err == MIMI_OK) {
            cJSON_Delete(resp);
            cJSON_Delete(root);
            strncpy(output, buf, output_size - 1);
            output[output_size - 1] = '\0';
            return MIMI_OK;
        }
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", mimi_err_to_name(err));
    }

    write_json(output, output_size, resp, err);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    if (err != MIMI_OK) {
        MIMI_LOGW(TAG, "subagents action=%s failed: %s", action, mimi_err_to_name(err));
    }
    return (err == MIMI_OK) ? MIMI_OK : err;
}

