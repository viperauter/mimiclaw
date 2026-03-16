#include "agent/subagent/subagent_runtime.h"
#include "mimi_config.h"

#if MIMI_ENABLE_SUBAGENT
#include "agent/agent_async.h"
#include "services/llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "log.h"
#include "tools/tool_registry.h"
#include "os/os.h"
#endif

#include <stdio.h>
#include <string.h>

static const char *TAG = "subagent_rt";

static bool csv_contains_token(const char *csv, const char *token)
{
    if (!csv || !csv[0] || !token || !token[0]) return false;
    char buf[256];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    for (char *p = strtok_r(buf, ",", &saveptr); p != NULL; p = strtok_r(NULL, ",", &saveptr)) {
        while (*p == ' ' || *p == '\t') p++;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
            p[--n] = '\0';
        }
        if (n == 0) continue;
        if (strcmp(p, token) == 0) return true;
    }
    return false;
}

mimi_err_t subagent_run(const char *role,
                        const subagent_request_t *req,
                        subagent_result_t *out,
                        const mimi_session_ctx_t *session_ctx)
{
#if !MIMI_ENABLE_SUBAGENT
    (void)role;
    (void)req;
    (void)session_ctx;
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->error, sizeof(out->error), "subagent disabled (MIMI_ENABLE_SUBAGENT=0)");
    }
    return MIMI_ERR_NOT_SUPPORTED;
#else
    if (!role || !req || !out) {
        return MIMI_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const subagent_runtime_config_t *cfg = subagent_get_by_role(role);
    if (!cfg) {
        MIMI_LOGW(TAG, "Subagent not found for role: %s", role);
        snprintf(out->error, sizeof(out->error),
                 "Subagent not found for role '%s'", role);
        return MIMI_ERR_NOT_FOUND;
    }

    const char *system_prompt = (cfg->system_prompt[0] != '\0') ? cfg->system_prompt : "";
    const char *tools_json = cfg->tools_json ? cfg->tools_json : tool_registry_get_tools_json();
    const int max_iters = (cfg->cfg.max_iters > 0) ? cfg->cfg.max_iters : 10;
    const int timeout_sec = (cfg->cfg.timeout_sec > 0) ? cfg->cfg.timeout_sec : 0;
    const uint64_t start_ms = mimi_time_ms();
    const uint64_t deadline_ms = (timeout_sec > 0) ? (start_ms + (uint64_t)timeout_sec * 1000ULL) : 0;

    cJSON *messages = cJSON_CreateArray();
    if (!messages) {
        return MIMI_ERR_NO_MEM;
    }

    char user_content[8192];
    if (req->context[0]) {
        /* Truncate safely to avoid large task/context blowing up stack buffer. */
        snprintf(user_content, sizeof(user_content),
                 "%.*s\n\n## Context\n%.*s",
                 3500, req->task,
                 3500, req->context);
    } else {
        snprintf(user_content, sizeof(user_content), "%.*s", 7000, req->task);
    }

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_content);
    cJSON_AddItemToArray(messages, user_msg);

    for (int iter = 0; iter < max_iters; iter++) {
        if (timeout_sec > 0 && mimi_time_ms() > deadline_ms) {
            snprintf(out->error, sizeof(out->error), "Subagent timed out (%ds)", timeout_sec);
            out->ok = false;
            cJSON_Delete(messages);
            return MIMI_ERR_TIMEOUT;
        }

        llm_response_t resp;
        memset(&resp, 0, sizeof(resp));

        llm_chat_req_t req = {
            .system_prompt = system_prompt,
            .messages = messages,
            .tools_json = tools_json,
            .trace_id = NULL,
        };
        mimi_err_t err = llm_chat_tools_req(&req, &resp);
        if (err != MIMI_OK) {
            snprintf(out->error, sizeof(out->error),
                     "Subagent LLM error: %s", mimi_err_to_name(err));
            llm_response_free(&resp);
            cJSON_Delete(messages);
            return err;
        }

        /* If no tool_use, we are done. */
        if (!resp.tool_use) {
            if (resp.text && resp.text_len > 0) {
                strncpy(out->content, resp.text, sizeof(out->content) - 1);
                out->content[sizeof(out->content) - 1] = '\0';
                out->ok = true;
            } else {
                snprintf(out->error, sizeof(out->error), "Subagent produced no text response");
                out->ok = false;
            }
            llm_response_free(&resp);
            cJSON_Delete(messages);
            return MIMI_OK;
        }

        /* Append assistant message (OpenAI tool-calling format). */
        cJSON *asst_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(asst_msg, "role", "assistant");
        cJSON_AddStringToObject(asst_msg, "content", resp.text ? resp.text : "");

        cJSON *tool_calls = cJSON_CreateArray();
        for (int i = 0; i < resp.call_count; i++) {
            const llm_tool_call_t *call = &resp.calls[i];
            cJSON *tool_call = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_call, "id", call->id[0] ? call->id : "");
            cJSON_AddStringToObject(tool_call, "type", "function");

            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", call->name[0] ? call->name : "");
            cJSON_AddStringToObject(fn, "arguments", call->input ? call->input : "{}");
            cJSON_AddItemToObject(tool_call, "function", fn);

            cJSON_AddItemToArray(tool_calls, tool_call);
        }
        cJSON_AddItemToObject(asst_msg, "tool_calls", tool_calls);
        cJSON_AddItemToArray(messages, asst_msg);

        /* Execute tools synchronously and append tool result messages. */
        for (int i = 0; i < resp.call_count; i++) {
            const llm_tool_call_t *call = &resp.calls[i];
            char tool_out[TOOL_OUTPUT_SIZE];
            tool_out[0] = '\0';

            const char *input = (call->input && call->input[0]) ? call->input : "{}";
            mimi_err_t te = MIMI_OK;
            if (cfg->cfg.tools[0] && !csv_contains_token(cfg->cfg.tools, call->name)) {
                te = MIMI_ERR_PERMISSION_DENIED;
                snprintf(tool_out, sizeof(tool_out),
                         "Error: tool '%s' is not allowed for subagent '%s'",
                         call->name, role);
            } else {
                te = tool_registry_execute(call->name, input,
                                           tool_out, sizeof(tool_out),
                                           session_ctx);
            }
            (void)te;

            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", call->id[0] ? call->id : "");
            cJSON_AddStringToObject(tool_msg, "content", tool_out[0] ? tool_out : "{}");
            cJSON_AddItemToArray(messages, tool_msg);
        }

        llm_response_free(&resp);
    }

    snprintf(out->error, sizeof(out->error), "Max iterations reached (%d)", max_iters);
    out->ok = false;
    cJSON_Delete(messages);
    return MIMI_ERR_TIMEOUT;
#endif
}

