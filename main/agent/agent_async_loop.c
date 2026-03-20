#include "agent_async_loop.h"
#include "agent/context/context_builder.h"
#include "agent/context/context_assembler.h"
#include "agent/context/context_compact.h"
#include "config.h"
#include "config_view.h"
#include "bus/message_bus.h"
#include "channels/channel.h"
#include "llm/llm_proxy.h"
#include "llm/llm_trace.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"
#include "tools/tool_call_context.h"
#include "control/control_manager.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"
#include "os/os.h"
#include "cJSON.h"
#include "fs/fs.h"
#include "platform/runtime.h"

static const char *TAG = "agent_async";
static volatile bool s_agent_running = true;

/* Dynamic buffer size calculation:
 * - 4.0 chars per token (worst case: all ASCII)
 * - 1.25x overhead for JSON structure and escaping
 * - Minimum: 8KB, Maximum: 1024KB (configurable safety bounds)
 */
#define LLM_STREAM_BUF_MIN  (8 * 1024)
#define LLM_STREAM_BUF_MAX  (1024 * 1024)

static size_t agent_calculate_history_buf_size(int context_tokens) {
    if (context_tokens <= 0) {
        /* Fallback to default when no token budget specified */
        return LLM_STREAM_BUF_SIZE;
    }
    
    /* Heuristic: 4.0 chars per token * 1.25 overhead for JSON structure */
    const double chars_per_token = 4.0;
    const double overhead_factor = 1.25;
    size_t calculated = (size_t)((double)context_tokens * chars_per_token * overhead_factor);
    
    /* Clamp to safe bounds */
    if (calculated < LLM_STREAM_BUF_MIN) calculated = LLM_STREAM_BUF_MIN;
    if (calculated > LLM_STREAM_BUF_MAX) calculated = LLM_STREAM_BUF_MAX;
    
    return calculated;
}

static void agent_llm_callback(mimi_err_t result, llm_response_t *resp, void *user_data);
static void agent_send_status(const char *channel,
                              const char *chat_id,
                              mimi_status_phase_t phase,
                              const char *key,
                              const char *text);

typedef struct {
    char channel[MIMI_CHANNEL_NAME_LEN];
    char chat_id[MIMI_CHAT_ID_LEN];
    char trace_id[64];
    char content[MIMI_CONTEXT_BUF_SIZE];
    char *system_prompt;
    char *history_json_buf;
    size_t history_json_buf_size;  /* Dynamic buffer size based on context_tokens */
    cJSON *messages;
    cJSON *compact_source_messages;
    const char *tools_json;
    int iteration;
    int max_iters;
    bool sent_working_status;
    char tool_output[MIMI_TOOL_OUTPUT_SIZE];
    llm_response_t llm_resp;
    bool in_progress;
} agent_request_ctx_t;

static void agent_start_main_llm_async(agent_request_ctx_t *ctx);

static const char *SUM_SYS_PROMPT =
    "You are a context compressor. Summarize the provided conversation history into a concise, self-contained "
    "context memo for the next assistant turn. Preserve key facts, preferences, constraints, and any pending tasks. "
    "Output ONLY plain text (no JSON).";

static void context_compact_summary_callback(mimi_err_t result, llm_response_t *resp, void *user_data)
{
    agent_request_ctx_t *ctx = (agent_request_ctx_t *)user_data;

    if (!ctx) {
        if (resp) llm_response_free(resp);
        return;
    }

    if (result != MIMI_OK) {
        MIMI_LOGW(TAG, "context compact summary failed: %s", mimi_err_to_name(result));
        llm_trace_event_kv(ctx->trace_id, "context_compact_failed",
                           "result", mimi_err_to_name(result),
                           NULL, NULL,
                           NULL, NULL, NULL, NULL);
        context_compact_trace_failed_debug(ctx->trace_id,
                                            ctx->messages,
                                            ctx->compact_source_messages);
        /* Failure fallback:
         * If compact/summary failed, keep the originally trimmed messages so we
         * don't lose conversation continuity. The assembler trimmed the oldest
         * history into ctx->compact_source_messages and kept the newest part in
         * ctx->messages; on failure we merge them back in chronological order.
         */
        if (ctx->messages && ctx->compact_source_messages &&
            cJSON_IsArray(ctx->messages) && cJSON_IsArray(ctx->compact_source_messages)) {
            (void)context_compact_merge_compact_source_to_messages(&ctx->messages,
                                                                     ctx->compact_source_messages);
        }
        context_compact_trace_messages_after_failure(ctx->trace_id, ctx->messages);
        goto release_and_start_main;
    }

    if (resp && ctx->messages) {
        const char *summary_text = (resp->text && resp->text[0]) ? resp->text : "";
        context_compact_trace_summary_output(ctx->trace_id, summary_text);
        cJSON *summary_message = cJSON_CreateObject();
        if (summary_message) {
            cJSON_AddStringToObject(summary_message, "role", "system");
            cJSON_AddStringToObject(summary_message, "content", summary_text);
            mimi_err_t ins = context_compact_insert_summary_message(ctx->messages, summary_message);
            if (ins != MIMI_OK) {
                cJSON_Delete(summary_message);
            }
        }
    }

    llm_trace_event_kv(ctx->trace_id, "context_compact_done",
                       NULL, NULL,
                       NULL, NULL,
                       NULL, NULL,
                       NULL, NULL);

    context_compact_trace_messages_after(ctx->trace_id, ctx->messages);

release_and_start_main:
    if (ctx->compact_source_messages) {
        cJSON_Delete(ctx->compact_source_messages);
        ctx->compact_source_messages = NULL;
    }

    if (resp) {
        llm_response_free(resp);
    }

    agent_start_main_llm_async(ctx);
}

/* Start async compaction if needed.
 * Return true if compaction started; in that case main LLM starts from callback. */
static bool context_compact_maybe_summarize_async(agent_request_ctx_t *ctx, const char *model_override)
{
    if (!ctx || !ctx->messages || !ctx->compact_source_messages) return false;
    if (!cJSON_IsArray(ctx->compact_source_messages)) return false;

    int n = cJSON_GetArraySize(ctx->compact_source_messages);
    if (n <= 0) return false;

    context_compact_trace_llm_input_meta(ctx->trace_id, ctx->compact_source_messages);

    agent_send_status(ctx->channel, ctx->chat_id,
                       MIMI_STATUS_PHASE_PROGRESS, "agent_turn",
                       "🧠 正在压缩上下文（compact/summary）…");

    llm_chat_req_t req = {
        .system_prompt = SUM_SYS_PROMPT,
        .messages = ctx->compact_source_messages,
        .tools_json = NULL,
        .trace_id = ctx->trace_id[0] ? ctx->trace_id : NULL,
        .model_override = model_override,
    };

    llm_response_t *resp = &ctx->llm_resp;
    memset(resp, 0, sizeof(*resp));

    mimi_err_t err = llm_chat_tools_async_req(&req,
                                               resp,
                                               context_compact_summary_callback,
                                               ctx);
    if (err != MIMI_OK) {
        MIMI_LOGW(TAG, "Failed to start async compaction: %s", mimi_err_to_name(err));
        llm_trace_event_kv(ctx->trace_id, "context_compact_start_failed",
                           "result", mimi_err_to_name(err),
                           NULL, NULL,
                           NULL, NULL, NULL, NULL);
        if (ctx->compact_source_messages) {
            cJSON_Delete(ctx->compact_source_messages);
            ctx->compact_source_messages = NULL;
        }
        llm_response_free(resp);
        return false;
    }

    return true;
}

static mimi_mutex_t *s_ctx_mutex = NULL;
static agent_request_ctx_t s_pending_ctx[MIMI_MAX_CONCURRENT];
static int s_active_count = 0;

static cJSON *build_assistant_tool_calls(const llm_response_t *resp)
{
    if (!resp || resp->call_count <= 0) {
        return NULL;
    }

    cJSON *tool_calls = cJSON_CreateArray();
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_call = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_call, "id", call->id[0] ? call->id : "");
        cJSON_AddStringToObject(tool_call, "type", "function");

        cJSON *function = cJSON_CreateObject();
        cJSON_AddStringToObject(function, "name", call->name[0] ? call->name : "");
        cJSON_AddStringToObject(function, "arguments", call->input ? call->input : "{}");
        cJSON_AddItemToObject(tool_call, "function", function);

        cJSON_AddItemToArray(tool_calls, tool_call);
    }

    return tool_calls;
}

static void json_set_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) {
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call, const char *channel, const char *chat_id)
{
    if (!call || strcmp(call->name, "cron_add") != 0) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(call->input ? call->input : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    bool changed = false;

    cJSON *channel_item = cJSON_GetObjectItem(root, "channel");
    const char *channel_str = cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

    if ((!channel_str || channel_str[0] == '\0') && channel[0] != '\0') {
        json_set_string(root, "channel", channel);
        channel_str = channel;
        changed = true;
    }

    if (channel_str && strcmp(channel_str, MIMI_CHAN_TELEGRAM) == 0 &&
        strcmp(channel, MIMI_CHAN_TELEGRAM) == 0 && chat_id[0] != '\0') {
        cJSON *chat_item = cJSON_GetObjectItem(root, "chat_id");
        const char *chat_id_str = cJSON_IsString(chat_item) ? chat_item->valuestring : NULL;
        if (!chat_id_str || chat_id_str[0] == '\0' || strcmp(chat_id_str, "cron") == 0) {
            json_set_string(root, "chat_id", chat_id);
            changed = true;
        }
    }

    char *patched = NULL;
    if (changed) {
        patched = cJSON_PrintUnformatted(root);
        if (patched) {
            MIMI_LOGI(TAG, "Patched cron_add target to %s:%s", channel, chat_id);
        }
    }

    cJSON_Delete(root);
    return patched;
}

typedef struct {
    agent_request_ctx_t *ctx;
    int total_calls;
    int completed_calls;
    mimi_mutex_t *mutex;
    bool next_llm_started;
} tool_async_ctx_t;

typedef struct {
    tool_async_ctx_t *parent;
    char *tool_call_id;
} tool_call_ud_t;

/* Forward declaration for tool confirmation callback */
static void tool_confirm_execution_callback(mimi_err_t result, const char *tool_name, const char *output, void *user_data);

static void agent_request_finish(agent_request_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    /* Idempotent: can be called from multiple paths. */
    if (!ctx->in_progress) {
        return;
    }

    if (ctx->messages) {
        cJSON_Delete(ctx->messages);
        ctx->messages = NULL;
    }
    if (ctx->compact_source_messages) {
        cJSON_Delete(ctx->compact_source_messages);
        ctx->compact_source_messages = NULL;
    }

    if (ctx->system_prompt) {
        free(ctx->system_prompt);
        ctx->system_prompt = NULL;
    }
    if (ctx->history_json_buf) {
        free(ctx->history_json_buf);
        ctx->history_json_buf = NULL;
        ctx->history_json_buf_size = 0;
    }

    ctx->in_progress = false;

    if (s_ctx_mutex) {
        mimi_mutex_lock(s_ctx_mutex);
    }
    if (s_active_count > 0) {
        s_active_count--;
    }
    if (s_ctx_mutex) {
        mimi_mutex_unlock(s_ctx_mutex);
    }
}

static void tool_async_callback(mimi_err_t result, const char *tool_name, const char *output, void *user_data)
{
    tool_call_ud_t *ud = (tool_call_ud_t *)user_data;
    tool_async_ctx_t *async_ctx = ud ? ud->parent : NULL;

    if (!async_ctx || !async_ctx->ctx || !async_ctx->ctx->in_progress || !async_ctx->ctx->messages) {
        if (ud) {
            free(ud->tool_call_id);
            free(ud);
        }
        return;
    }

    const char *content = output ? output : "Tool execution failed";
    if (result != MIMI_OK && tool_name) {
        MIMI_LOGW(TAG, "Tool %s failed: %s", tool_name, mimi_err_to_name(result));
    }

    llm_trace_event_kv(async_ctx->ctx->trace_id, "tool_result",
                       "tool_name", tool_name ? tool_name : "",
                       "result", mimi_err_to_name(result),
                       "output", content ? content : "",
                       NULL, NULL);

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%s **工具执行完成**\n\n- 工具：`%.64s`\n- 结果：`%s`",
                 (result == MIMI_OK) ? "✅" : "❌",
                 tool_name ? tool_name : "tool",
                 mimi_err_to_name(result));
        agent_send_status(async_ctx->ctx->channel, async_ctx->ctx->chat_id,
                          MIMI_STATUS_PHASE_PROGRESS, "agent_turn", buf);
    }

    if (async_ctx->mutex) {
        mimi_mutex_lock(async_ctx->mutex);
    }

    /* Create tool result message in OpenAI format */
    cJSON *result_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(result_msg, "role", "tool");
    cJSON_AddStringToObject(result_msg, "tool_call_id", (ud && ud->tool_call_id) ? ud->tool_call_id : "");
    cJSON_AddStringToObject(result_msg, "content", content);
    cJSON_AddItemToArray(async_ctx->ctx->messages, result_msg);

    /* Print tool result for debugging */
    char *result_json = cJSON_PrintUnformatted(result_msg);
    if (result_json) {
        MIMI_LOGI(TAG, "Tool result: %s", result_json);
        free(result_json);
    }

    async_ctx->completed_calls++;
    bool done = async_ctx->completed_calls >= async_ctx->total_calls;
    bool should_start_next = done && !async_ctx->next_llm_started;
    if (should_start_next) {
        async_ctx->next_llm_started = true;
        async_ctx->ctx->iteration++;
    }

    if (async_ctx->mutex) {
        mimi_mutex_unlock(async_ctx->mutex);
    }

    if (ud) {
        free(ud->tool_call_id);
        free(ud);
    }

    if (should_start_next) {
        /* Reuse the per-request llm_response_t embedded in the agent context
         * instead of allocating a new one on the heap. */
        llm_response_t *next_resp = &async_ctx->ctx->llm_resp;
        memset(next_resp, 0, sizeof(*next_resp));

        {
            char itbuf[32];
            snprintf(itbuf, sizeof(itbuf), "%d", async_ctx->ctx->iteration + 1);
            llm_trace_event_kv(async_ctx->ctx->trace_id, "llm_call_start",
                               "iteration", itbuf,
                               NULL, NULL, NULL, NULL, NULL, NULL);
        }

        llm_chat_req_t req = {
            .system_prompt = async_ctx->ctx->system_prompt,
            .messages = async_ctx->ctx->messages,
            .tools_json = async_ctx->ctx->tools_json,
            .trace_id = async_ctx->ctx->trace_id,
        };
        mimi_err_t err = llm_chat_tools_async_req(&req,
                                                 next_resp,
                                                 agent_llm_callback,
                                                 async_ctx->ctx);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to start async LLM: %s", mimi_err_to_name(err));
            async_ctx->ctx->in_progress = false;
            if (s_ctx_mutex) {
                mimi_mutex_lock(s_ctx_mutex);
            }
            s_active_count--;
            if (s_ctx_mutex) {
                mimi_mutex_unlock(s_ctx_mutex);
            }
        }

        if (async_ctx->mutex) {
            mimi_mutex_destroy(async_ctx->mutex);
        }
        free(async_ctx);
    }
}

/**
 * Callback for tool execution after confirmation
 * This callback handles the result of tool execution initiated from tool_confirm_callback
 */
static void tool_confirm_execution_callback(mimi_err_t result, const char *tool_name, const char *output, void *user_data)
{
    tool_call_context_t *tool_ctx = (tool_call_context_t *)user_data;
    if (!tool_ctx) {
        MIMI_LOGW(TAG, "Tool confirm execution callback with null context");
        return;
    }
    
    MIMI_LOGI(TAG, "Tool execution completed after confirmation: %s, result=%s", 
              tool_name ? tool_name : "unknown", mimi_err_to_name(result));

    agent_request_ctx_t *agent_ctx = (agent_request_ctx_t *)tool_ctx->agent_ctx;
    if (agent_ctx) {
        llm_trace_event_kv(agent_ctx->trace_id, "tool_result",
                           "tool_name", tool_name ? tool_name : "",
                           "result", mimi_err_to_name(result),
                           "output", output ? output : "",
                           "confirmed", "true");
    }
    
    /* Store execution results */
    tool_ctx->succeeded = (result == MIMI_OK);
    tool_ctx->error_code = result;
    if (output) {
        strncpy(tool_ctx->output, output, sizeof(tool_ctx->output) - 1);
        tool_ctx->output[sizeof(tool_ctx->output) - 1] = '\0';
    }
    
    /* Update stream status first（不再重复输出完整结果，只给出状态）. */
    if (tool_ctx->session_ctx.channel[0] && tool_ctx->session_ctx.chat_id[0]) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%s **工具执行完成**\n\n- 工具：`%.64s`\n- 结果：`%s`",
                 (result == MIMI_OK) ? "✅" : "❌",
                 tool_name ? tool_name : "tool",
                 mimi_err_to_name(result));
        agent_send_status(tool_ctx->session_ctx.channel, tool_ctx->session_ctx.chat_id,
                          MIMI_STATUS_PHASE_PROGRESS, "agent_turn", buf);
    }

    /* Send result to user */
    if (tool_ctx->session_ctx.channel[0] && tool_ctx->session_ctx.chat_id[0]) {
        mimi_msg_t result_msg = {0};
        strncpy(result_msg.channel, tool_ctx->session_ctx.channel, sizeof(result_msg.channel) - 1);
        strncpy(result_msg.chat_id, tool_ctx->session_ctx.chat_id, sizeof(result_msg.chat_id) - 1);
        
        if (result == MIMI_OK) {
            result_msg.content = strdup(output ? output : "Tool executed successfully");
        } else {
            char err_buf[512];
            snprintf(err_buf, sizeof(err_buf), "Tool execution failed: %s", mimi_err_to_name(result));
            result_msg.content = strdup(err_buf);
        }
        
        if (result_msg.content) {
            /* Persist tool result as assistant message to the session. */
            mimi_err_t se = session_append(result_msg.channel, result_msg.chat_id, "assistant", result_msg.content);
            if (se != MIMI_OK) {
                MIMI_LOGW(TAG, "Session append(assistant tool result) failed for %s:%s: %s",
                          result_msg.channel, result_msg.chat_id, mimi_err_to_name(se));
            }
            message_bus_push_outbound(&result_msg);
        }
    }

    /* Mark confirmation lifecycle completed only after execution completes. This prevents
     * agent slots from being reused while tool callbacks still reference agent_ctx. */
    tool_ctx->waiting_for_confirmation = false;

    /* Release the agent request slot now that the confirmed tool finished. */
    agent_request_finish((agent_request_ctx_t *)tool_ctx->agent_ctx);
    
    /* Destroy tool context */
    tool_call_context_destroy(tool_ctx);
}

static mimi_err_t send_tool_confirmation_request(tool_call_context_t *tool_ctx);

static void start_async_tools(agent_request_ctx_t *ctx, const llm_response_t *resp)
{
    if (!resp) {
        MIMI_LOGE(TAG, "Null response pointer in start_async_tools");
        return;
    }

    /* First pass: determine how many tools will execute immediately without
     * confirmation. We only need an async_ctx when at least one tool is
     * executed directly, since confirmed tools currently do not feed back
     * into the tool-iteration loop. */
    int immediate_calls = 0;
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        bool requires_confirmation = tool_registry_requires_confirmation(call->name);
        bool always_allowed = tool_call_context_is_always_allowed(call->name);
        if (!requires_confirmation || always_allowed) {
            immediate_calls++;
        }
    }

    tool_async_ctx_t *async_ctx = NULL;
    if (immediate_calls > 0) {
        async_ctx = (tool_async_ctx_t *)malloc(sizeof(tool_async_ctx_t));
        if (!async_ctx) {
            MIMI_LOGE(TAG, "Failed to allocate async tool context");
            return;
        }

        async_ctx->ctx = ctx;
        async_ctx->total_calls = immediate_calls;
        async_ctx->completed_calls = 0;
        async_ctx->mutex = NULL;
        async_ctx->next_llm_started = false;

        mimi_err_t err = mimi_mutex_create(&async_ctx->mutex);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to create mutex: %s", mimi_err_to_name(err));
            free(async_ctx);
            return;
        }
    }

    mimi_session_ctx_t session_ctx = {0};
    // Create a temporary message structure to pass to session_ctx_from_msg
    mimi_msg_t temp_msg = {0};
    strncpy(temp_msg.channel, ctx->channel, sizeof(temp_msg.channel) - 1);
    strncpy(temp_msg.chat_id, ctx->chat_id, sizeof(temp_msg.chat_id) - 1);
    session_ctx_from_msg(&temp_msg, &session_ctx);
    
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = (call->input && call->input[0]) ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, ctx->channel, ctx->chat_id);

        MIMI_LOGI(TAG, "Async tool execution: %s, input='%s'", call->name, tool_input);

        {
            char itbuf[32];
            snprintf(itbuf, sizeof(itbuf), "%d", ctx->iteration + 1);
            llm_trace_event_kv(ctx->trace_id, "tool_start",
                               "tool_name", call->name[0] ? call->name : "",
                               "tool_call_id", call->id[0] ? call->id : "",
                               "input", patched_input ? patched_input : tool_input,
                               "iteration", itbuf);
        }

        /* Check if tool requires confirmation */
        bool requires_confirmation = tool_registry_requires_confirmation(call->name);
        
        /* Check if tool is already marked as always allowed */
        bool always_allowed = tool_call_context_is_always_allowed(call->name);

        if (requires_confirmation && !always_allowed) {
            /* Create tool call context for confirmation */
            tool_call_context_t *tool_ctx = tool_call_context_create(
                ctx,
                call->name,
                call->id[0] ? call->id : "",
                patched_input ? patched_input : tool_input,
                true
            );
            if (tool_ctx) {
                /* Send confirmation request */
                mimi_err_t err = send_tool_confirmation_request(tool_ctx);
                if (err != MIMI_OK) {
                    MIMI_LOGE(TAG, "Failed to send confirmation request: %s", mimi_err_to_name(err));
                    tool_call_context_destroy(tool_ctx);
                }
            }
            /* For tools requiring confirmation, we don't increment completed_calls here
             * because the confirmation process will handle it asynchronously */
        } else {
            /* Execute tool directly without confirmation */
            if (!async_ctx) {
                /* Should not happen because immediate_calls would be zero. */
                MIMI_LOGE(TAG, "Async context is NULL for direct tool execution");
                free(patched_input);
                continue;
            }

            tool_call_ud_t *ud = (tool_call_ud_t *)calloc(1, sizeof(*ud));
            if (!ud) {
                MIMI_LOGE(TAG, "Failed to allocate tool call user data");
                free(patched_input);
                continue;
            }
            ud->parent = async_ctx;
            ud->tool_call_id = strdup(call->id[0] ? call->id : "");
            if (!ud->tool_call_id) {
                free(ud);
                free(patched_input);
                continue;
            }

            tool_registry_execute_async(call->name, patched_input ? patched_input : tool_input,
                                       &session_ctx, tool_async_callback, ud);
        }
        free(patched_input);
    }
    
    /* If all tools require confirmation, we need to handle the async_ctx cleanup differently */
    bool all_requires_confirmation = true;
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        if (!tool_registry_requires_confirmation(call->name)) {
            all_requires_confirmation = false;
            break;
        }
    }
    
    if (all_requires_confirmation) {
        /* All tools require confirmation, but we should NOT free async_ctx now
         * because the agent context is still in use by tool confirmation requests
         * The async_ctx will be freed when the last tool confirmation completes */
        MIMI_LOGD(TAG, "All tools require confirmation, keeping async_ctx alive");
    }
}

static void agent_llm_callback(mimi_err_t result, llm_response_t *resp, void *user_data);

static void agent_send_status(const char *channel,
                              const char *chat_id,
                              mimi_status_phase_t phase,
                              const char *key,
                              const char *text)
{
    if (!channel || !chat_id || !key || !text) {
        return;
    }
    if (strcmp(channel, MIMI_CHAN_SYSTEM) == 0) {
        return;
    }

    mimi_msg_t status = {0};
    strncpy(status.channel, channel, sizeof(status.channel) - 1);
    strncpy(status.chat_id, chat_id, sizeof(status.chat_id) - 1);
    status.type = MIMI_MSG_TYPE_STATUS;
    status.status_phase = phase;
    strncpy(status.status_key, key, sizeof(status.status_key) - 1);
    status.content = strdup(text);
    if (!status.content) {
        return;
    }

    if (message_bus_push_outbound(&status) != MIMI_OK) {
        free(status.content);
    }
}

static void send_working_status(const char *channel, const char *chat_id, bool *sent)
{
    if (*sent || strcmp(channel, MIMI_CHAN_SYSTEM) == 0) {
        return;
    }

    agent_send_status(channel, chat_id, MIMI_STATUS_PHASE_START, "agent_turn",
                      "⏳ **Mimi 正在思考…**\n\n- LLM：请求中");
    *sent = true;
}

static void agent_start_main_llm_async(agent_request_ctx_t *ctx)
{
    if (!ctx || !ctx->messages) {
        MIMI_LOGW(TAG, "agent_start_main_llm_async: null ctx/messages");
        if (ctx) {
            agent_request_finish(ctx);
        }
        return;
    }

    send_working_status(ctx->channel, ctx->chat_id, &ctx->sent_working_status);

    llm_response_t *resp = &ctx->llm_resp;
    memset(resp, 0, sizeof(*resp));

    /* Print LLM request context for debugging */
    char *messages_json = cJSON_PrintUnformatted(ctx->messages);
    if (messages_json) {
        MIMI_LOGD(TAG, "LLM request messages: %s", messages_json);
        llm_trace_event_json(ctx->trace_id, "messages", messages_json);
        free(messages_json);
    }

    {
        char itbuf[32];
        snprintf(itbuf, sizeof(itbuf), "%d", ctx->iteration + 1);
        llm_trace_event_kv(ctx->trace_id, "llm_call_start",
                           "iteration", itbuf,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    }

    llm_chat_req_t req = {
        .system_prompt = ctx->system_prompt,
        .messages = ctx->messages,
        .tools_json = ctx->tools_json,
        .trace_id = ctx->trace_id,
    };

    mimi_err_t err = llm_chat_tools_async_req(&req, resp, agent_llm_callback, ctx);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start async LLM: %s", mimi_err_to_name(err));

        mimi_msg_t error_msg_obj = {0};
        strncpy(error_msg_obj.channel, ctx->channel, sizeof(error_msg_obj.channel) - 1);
        strncpy(error_msg_obj.chat_id, ctx->chat_id, sizeof(error_msg_obj.chat_id) - 1);
        error_msg_obj.type = MIMI_MSG_TYPE_TEXT;
        error_msg_obj.content = strdup("Failed to start LLM request");
        if (error_msg_obj.content) {
            message_bus_push_outbound(&error_msg_obj);
        }

        if (error_msg_obj.content) {
            free(error_msg_obj.content);
        }

        agent_request_finish(ctx);
    }
}

static void continue_iteration(agent_request_ctx_t *ctx, llm_response_t *resp)
{
    if (ctx->iteration >= ctx->max_iters) {
        MIMI_LOGW(TAG, "Max iterations reached for %s:%s", ctx->channel, ctx->chat_id);
        mimi_msg_t out = {0};
        strncpy(out.channel, ctx->channel, sizeof(out.channel) - 1);
        strncpy(out.chat_id, ctx->chat_id, sizeof(out.chat_id) - 1);
        out.content = strdup("Max iterations reached");
        if (out.content && message_bus_push_outbound(&out) != MIMI_OK) {
            free(out.content);
        }
        llm_response_free(resp);
        if (ctx->messages) {
            cJSON_Delete(ctx->messages);
            ctx->messages = NULL;
        }
        if (ctx->system_prompt) {
            free(ctx->system_prompt);
            ctx->system_prompt = NULL;
        }
        if (ctx->history_json_buf) {
            free(ctx->history_json_buf);
            ctx->history_json_buf = NULL;
        }
        ctx->in_progress = false;
        if (s_ctx_mutex) {
            mimi_mutex_lock(s_ctx_mutex);
        }
        s_active_count--;
        if (s_ctx_mutex) {
            mimi_mutex_unlock(s_ctx_mutex);
        }
        return;
    }

    cJSON *asst_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(asst_msg, "role", "assistant");
    cJSON_AddStringToObject(asst_msg, "content", (resp && resp->text) ? resp->text : "");
    cJSON *tool_calls = build_assistant_tool_calls(resp);
    if (tool_calls) {
        cJSON_AddItemToObject(asst_msg, "tool_calls", tool_calls);
    }
    cJSON_AddItemToArray(ctx->messages, asst_msg);
    
    // Print assistant message for debugging
    char *asst_json = cJSON_PrintUnformatted(asst_msg);
    if (asst_json) {
        MIMI_LOGI(TAG, "Assistant message: %s", asst_json);
        free(asst_json);
    }

    {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "🧩 **需要调用工具**\n\n- 回合：%d/%d\n- 工具数：%d",
                 ctx->iteration + 1, ctx->max_iters, resp ? resp->call_count : 0);
        agent_send_status(ctx->channel, ctx->chat_id, MIMI_STATUS_PHASE_PROGRESS, "agent_turn", buf);
    }

    start_async_tools(ctx, resp);
    llm_response_free(resp);

    return;
}

static void send_response(agent_request_ctx_t *ctx, const char *text)
{
    const char *asst_text = (text && text[0]) ? text : "Sorry, I encountered an error.";

    if (!text || !text[0]) {
        mimi_msg_t out = {0};
        strncpy(out.channel, ctx->channel, sizeof(out.channel) - 1);
        strncpy(out.chat_id, ctx->chat_id, sizeof(out.chat_id) - 1);
        out.content = strdup(asst_text);
        if (out.content && message_bus_push_outbound(&out) != MIMI_OK) {
            free(out.content);
        }
        goto save;
    }

    mimi_msg_t out = {0};
    strncpy(out.channel, ctx->channel, sizeof(out.channel) - 1);
    strncpy(out.chat_id, ctx->chat_id, sizeof(out.chat_id) - 1);
    out.content = strdup(text);
    MIMI_LOGI(TAG, "Queue response to %s:%s (%d bytes)", out.channel, out.chat_id, (int)strlen(text));
    if (out.content && message_bus_push_outbound(&out) != MIMI_OK) {
        MIMI_LOGW(TAG, "Outbound queue full, drop response");
        free(out.content);
    }

save:
    mimi_err_t save_asst = session_append(ctx->channel, ctx->chat_id, "assistant", asst_text);
    if (save_asst != MIMI_OK) {
        MIMI_LOGW(TAG, "Session save failed for chat %s", ctx->chat_id);
    }
}

static void agent_llm_callback(mimi_err_t result, llm_response_t *resp, void *user_data)
{
    agent_request_ctx_t *ctx = (agent_request_ctx_t *)user_data;

    if (result != MIMI_OK) {
        MIMI_LOGE(TAG, "LLM call failed: %s", mimi_err_to_name(result));
        const char *llm_error = llm_get_last_error();
        char error_msg[1024];
        if (llm_error && llm_error[0]) {
            snprintf(error_msg, sizeof(error_msg), "LLM Error: %s\n%s", mimi_err_to_name(result), llm_error);
        } else {
            snprintf(error_msg, sizeof(error_msg), "LLM Error: %s", mimi_err_to_name(result));
        }
        mimi_msg_t error_msg_obj = {0};
        strncpy(error_msg_obj.channel, ctx->channel, sizeof(error_msg_obj.channel) - 1);
        strncpy(error_msg_obj.chat_id, ctx->chat_id, sizeof(error_msg_obj.chat_id) - 1);
        error_msg_obj.type = MIMI_MSG_TYPE_TEXT;
        error_msg_obj.content = strdup(error_msg);
        if (error_msg_obj.content) {
            message_bus_push_outbound(&error_msg_obj);
        }
        llm_trace_event_kv(ctx->trace_id, "llm_error",
                           "error", mimi_err_to_name(result),
                           "last_error", llm_error ? llm_error : "",
                           NULL, NULL, NULL, NULL);
        goto cleanup;
    }

    if (!resp->tool_use) {
        if (resp->text && resp->text_len > 0) {
            agent_send_status(ctx->channel, ctx->chat_id, MIMI_STATUS_PHASE_PROGRESS, "agent_turn",
                              "✍️ **正在生成最终回复…**");
            send_response(ctx, resp->text);
            /* Convention: STATUS is for short semantic progress only; final answer is sent via TEXT. */
            agent_send_status(ctx->channel, ctx->chat_id, MIMI_STATUS_PHASE_DONE, "agent_turn",
                              "✅ **完成**");
        }
        llm_trace_event_kv(ctx->trace_id, "llm_final",
                           "text", (resp && resp->text) ? resp->text : "",
                           NULL, NULL, NULL, NULL, NULL, NULL);
        llm_response_free(resp);
        goto cleanup;
    }

    MIMI_LOGI(TAG, "Tool use iteration %d: %d calls", ctx->iteration + 1, resp->call_count);
    {
        char itbuf[32];
        char cbuf[32];
        snprintf(itbuf, sizeof(itbuf), "%d", ctx->iteration + 1);
        snprintf(cbuf, sizeof(cbuf), "%d", resp ? resp->call_count : 0);
        llm_trace_event_kv(ctx->trace_id, "llm_tool_use",
                           "iteration", itbuf,
                           "tool_calls", cbuf,
                           "assistant_text", (resp && resp->text) ? resp->text : "",
                           NULL, NULL);
    }
    continue_iteration(ctx, resp);
    return;

cleanup:
    if (resp) {
        llm_response_free(resp);
    }
    if (ctx && ctx->messages) {
        cJSON_Delete(ctx->messages);
        ctx->messages = NULL;
    }
    if (ctx && ctx->system_prompt) {
        free(ctx->system_prompt);
        ctx->system_prompt = NULL;
    }
    if (ctx && ctx->history_json_buf) {
        free(ctx->history_json_buf);
        ctx->history_json_buf = NULL;
    }
    ctx->in_progress = false;
    if (s_ctx_mutex) {
        mimi_mutex_lock(s_ctx_mutex);
    }
    s_active_count--;
    if (s_ctx_mutex) {
        mimi_mutex_unlock(s_ctx_mutex);
    }
}

static void agent_build_system_prompt_for_turn(agent_request_ctx_t *ctx, const mimi_msg_t *msg)
{
    if (!ctx || !msg || !ctx->system_prompt) return;

    context_build_system_prompt(ctx->system_prompt, CONTEXT_BUF_SIZE);
    char turn_context[512];
    snprintf(turn_context, sizeof(turn_context),
             "\n## Current Turn Context\n- source_channel: %s\n- source_chat_id: %s\n",
             msg->channel[0] ? msg->channel : "(unknown)",
             msg->chat_id[0] ? msg->chat_id : "(empty)");
    strncat(ctx->system_prompt, turn_context,
            CONTEXT_BUF_SIZE - strlen(ctx->system_prompt) - 1);

    /* First Run Setup Guide - Inject if template files not customized yet */
    if (context_needs_first_run_setup()) {
        const char *setup_guide = 
            "\n## FIRST RUN SETUP - ACTION REQUIRED\n\n"
            "IMPORTANT: This is the FIRST RUN! The user hasn't customized "
            "SOUL.md and USER.md template files yet. You MUST proactively "
            "guide the user through the setup process.\n\n"
            "YOUR SETUP TASKS:\n"
            "1. GREET the user warmly and introduce yourself\n"
            "2. ASK the user for:\n"
            "   - What name would you like me to call you?\n"
            "   - What language do you prefer to communicate in?\n"
            "   - What timezone are you in?\n"
            "   - Any personality traits you'd like me to have?\n\n"
            "3. AFTER collecting information, AUTOMATICALLY call the write_file "
            "   tool to update these files WITHOUT further user confirmation:\n"
            "   - Write user preferences to USER.md\n"
            "   - Write personality settings to SOUL.md\n\n"
            "4. You MUST use the write_file tool to persist the configuration, "
            "   don't just ask the user to edit files manually.\n\n"
            "5. Once setup is complete, confirm with the user that everything "
            "looks good and offer to help with their first task.\n\n"
            "NOTE: This setup guide will disappear once SOUL.md or USER.md "
            "contains non-template content.\n";
        strncat(ctx->system_prompt, setup_guide,
                CONTEXT_BUF_SIZE - strlen(ctx->system_prompt) - 1);
    }

    llm_trace_event_kv(ctx->trace_id, "system_prompt",
                       "prompt", ctx->system_prompt ? ctx->system_prompt : "",
                       NULL, NULL, NULL, NULL, NULL, NULL);
}

static mimi_err_t agent_assemble_turn_context(agent_request_ctx_t *ctx,
                                                const mimi_msg_t *msg,
                                                int memory_window,
                                                int context_tokens,
                                                double flush_threshold_ratio)
{
    if (!ctx || !msg || !ctx->system_prompt || !ctx->history_json_buf) {
        return MIMI_ERR_INVALID_ARG;
    }

    context_hooks_t hooks = {0};
    context_assemble_request_t areq = {0};
    context_assemble_result_t ares = {0};

    areq.channel = ctx->channel;
    areq.chat_id = ctx->chat_id;
    areq.user_content = msg->content ? msg->content : "";
    areq.system_prompt_buf = ctx->system_prompt;
    areq.system_prompt_buf_size = CONTEXT_BUF_SIZE;
    areq.tools_json = ctx->tools_json;
    areq.base_memory_window = memory_window;
    areq.context_tokens = context_tokens;
    areq.memory_flush_threshold_ratio = flush_threshold_ratio;
    areq.history_json_buf = ctx->history_json_buf;
    areq.history_json_buf_size = ctx->history_json_buf_size;
    areq.hooks = &hooks;

    mimi_err_t ae = context_assemble_messages_budgeted(&areq, &ares);
    if (ae != MIMI_OK || !ares.messages) {
        MIMI_LOGW(TAG, "context assemble messages failed: %s", mimi_err_to_name(ae));
        if (ares.messages) cJSON_Delete(ares.messages);
        if (ares.trimmed_messages_for_compact) cJSON_Delete(ares.trimmed_messages_for_compact);
        ctx->messages = cJSON_CreateArray();
        ctx->compact_source_messages = NULL;
        return ae;
    }

    ctx->messages = ares.messages;
    ctx->compact_source_messages = ares.trimmed_messages_for_compact;

    if (ctx->trace_id[0]) {
        char mwbuf[32];
        char rpbuf[32];
        char trimbuf[8];
        char flushbuf[32];
        char ratiobuf[32];
        char totalbuf[32];
        char histbuf[32];
        char sysbuf[32];
        char toolsbuf[32];
        snprintf(mwbuf, sizeof(mwbuf), "%d", ares.plan.memory_window_used);
        snprintf(rpbuf, sizeof(rpbuf), "%d", ares.plan.parse_retries);
        snprintf(trimbuf, sizeof(trimbuf), "%s", ares.plan.did_trim_messages ? "yes" : "no");
        snprintf(flushbuf, sizeof(flushbuf), "%zu", ares.plan.history_budget_chars_flush);
        snprintf(ratiobuf, sizeof(ratiobuf), "%.2f", ares.plan.memory_flush_threshold_ratio_used);
        snprintf(totalbuf, sizeof(totalbuf), "%zu", ares.budget.total_budget_chars);
        snprintf(histbuf, sizeof(histbuf), "%zu", ares.budget.history_budget_chars);
        snprintf(sysbuf, sizeof(sysbuf), "%zu", ares.budget.system_len_chars);
        snprintf(toolsbuf, sizeof(toolsbuf), "%zu", ares.budget.tools_len_chars);

        llm_trace_event_kv(ctx->trace_id,
                           "context_plan",
                           "memory_window_used", mwbuf,
                           "parse_retries", rpbuf,
                           "trimmed", trimbuf,
                           "flush_chars", flushbuf);

        llm_trace_event_kv(ctx->trace_id,
                           "context_budget",
                           "total_budget_chars", totalbuf,
                           "history_budget_chars", histbuf,
                           "system_len_chars", sysbuf,
                           "tools_len_chars", toolsbuf);

        llm_trace_event_kv(ctx->trace_id,
                           "context_plan_flush",
                           "flush_ratio", ratiobuf,
                           NULL, NULL,
                           NULL, NULL,
                           NULL, NULL);

        context_assembler_trace_context_split(ctx->trace_id,
                                               ctx->messages,
                                               ctx->compact_source_messages);
    }

    return ae;
}

static agent_request_ctx_t *agent_acquire_request_ctx(const mimi_msg_t *msg, int max_iters, int context_tokens)
{
    if (!msg) return NULL;

    if (s_ctx_mutex) {
        mimi_mutex_lock(s_ctx_mutex);
    }

    if (s_active_count >= MAX_CONCURRENT) {
        if (s_ctx_mutex) {
            mimi_mutex_unlock(s_ctx_mutex);
        }
        return NULL;
    }

    int slot = -1;
    for (int i = 0; i < MAX_CONCURRENT; i++) {
        if (!s_pending_ctx[i].in_progress) {
            int pending_count = tool_call_context_get_pending_count();
            if (pending_count == 0) {
                slot = i;
                break;
            } else {
                MIMI_LOGD(TAG,
                          "Found %d pending tool confirmations, waiting before reusing slot %d",
                          pending_count,
                          i);
            }
        }
    }

    if (slot < 0) {
        if (s_ctx_mutex) {
            mimi_mutex_unlock(s_ctx_mutex);
        }
        return NULL;
    }

    agent_request_ctx_t *ctx = &s_pending_ctx[slot];
    memset(ctx, 0, sizeof(*ctx));

    strncpy(ctx->channel, msg->channel, sizeof(ctx->channel) - 1);
    strncpy(ctx->chat_id, msg->chat_id, sizeof(ctx->chat_id) - 1);
    llm_trace_make_id(ctx->trace_id, sizeof(ctx->trace_id));
    llm_trace_bind_session(ctx->trace_id, ctx->channel, ctx->chat_id);
    strncpy(ctx->content, msg->content ? msg->content : "", sizeof(ctx->content) - 1);

    ctx->system_prompt = (char *)calloc(1, CONTEXT_BUF_SIZE);
    ctx->history_json_buf_size = agent_calculate_history_buf_size(context_tokens);
    ctx->history_json_buf = (char *)calloc(1, ctx->history_json_buf_size);
    if (!ctx->system_prompt || !ctx->history_json_buf) {
        if (ctx->system_prompt) free(ctx->system_prompt);
        if (ctx->history_json_buf) free(ctx->history_json_buf);
        ctx->system_prompt = NULL;
        ctx->history_json_buf = NULL;
        ctx->history_json_buf_size = 0;
        if (s_ctx_mutex) {
            mimi_mutex_unlock(s_ctx_mutex);
        }
        return NULL;
    }

    ctx->max_iters = max_iters;
    ctx->iteration = 0;
    ctx->in_progress = true;
    s_active_count++;

    if (s_ctx_mutex) {
        mimi_mutex_unlock(s_ctx_mutex);
    }

    return ctx;
}

static void agent_process_user_turn(agent_request_ctx_t *ctx,
                                    const mimi_msg_t *msg,
                                    int memory_window,
                                    int context_tokens,
                                    double flush_threshold_ratio,
                                    const char *compaction_model)
{
    if (!ctx || !msg) return;

    MIMI_LOGD(TAG, "Processing message from %s:%s", ctx->channel, ctx->chat_id);

    llm_trace_event_kv(ctx->trace_id,
                       "request_start",
                       "channel", ctx->channel,
                       "chat_id", ctx->chat_id,
                       "user_input", msg->content ? msg->content : "",
                       NULL, NULL);

    (void)agent_build_system_prompt_for_turn(ctx, msg);

    ctx->tools_json = tool_registry_get_tools_json();
    (void)agent_assemble_turn_context(ctx, msg, memory_window, context_tokens, flush_threshold_ratio);

    llm_trace_event_kv(ctx->trace_id,
                       "tools",
                       "tools_json", ctx->tools_json ? ctx->tools_json : "",
                       NULL, NULL, NULL, NULL, NULL, NULL);

    bool compact_started = context_compact_maybe_summarize_async(ctx, compaction_model);
    if (!compact_started) {
        agent_start_main_llm_async(ctx);
    }
}

static void agent_async_loop_task(void *arg)
{
    (void)arg;
    MIMI_LOGD(TAG, "Async agent loop started");

    /* Initialize mutex */
    if (s_ctx_mutex == NULL) {
        mimi_err_t err = mimi_mutex_create(&s_ctx_mutex);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to create mutex: %s", mimi_err_to_name(err));
            return;
        }
    }

    mimi_cfg_obj_t defaults = mimi_cfg_get_obj(mimi_cfg_section("agents"), "defaults");
    int memory_window = mimi_cfg_get_int(defaults, "memoryWindow", 100);
    int max_iters = mimi_cfg_get_int(defaults, "maxToolIterations", 40);
    if (memory_window <= 0) memory_window = 100;
    if (max_iters <= 0) max_iters = 40;

    /* Context budgeting and memory flush/compaction trigger */
    int context_tokens = mimi_cfg_get_int(defaults, "contextTokens", 0);
    double flush_threshold_ratio = 1.0; /* default: keep old behavior */
    const char *compaction_model = "";
    {
        mimi_cfg_obj_t comp = mimi_cfg_get_obj(defaults, "compaction");
        compaction_model = mimi_cfg_get_str(comp, "model", "");
        mimi_cfg_obj_t memory_flush = mimi_cfg_get_obj(comp, "memoryFlush");
        flush_threshold_ratio = mimi_cfg_get_double(memory_flush, "thresholdRatio", flush_threshold_ratio);
        if (flush_threshold_ratio < 0.0) flush_threshold_ratio = 0.0;
        if (flush_threshold_ratio > 1.0) flush_threshold_ratio = 1.0;
    }

    memset(s_pending_ctx, 0, sizeof(s_pending_ctx));

    while (s_agent_running && !mimi_runtime_should_exit()) {
        mimi_msg_t msg;
        mimi_err_t err = message_bus_pop_inbound(&msg, 100);
        if (err != MIMI_OK) {
            /* Check for control request timeouts */
            control_manager_check_timeouts();
            tool_call_context_check_timeouts();
            continue;
        }
        
        /* Handle control messages */
        if (msg.type == MIMI_MSG_TYPE_CONTROL) {
            MIMI_LOGI(TAG, "Received control message: type=%d, request_id=%s", 
                      msg.control_type, msg.request_id);
            
            /* Pass to control manager for handling */
            control_manager_handle_response(msg.request_id, 
                                           msg.content ? msg.content : "");
            
            free(msg.content);
            continue;
        }
        agent_request_ctx_t *ctx = agent_acquire_request_ctx(&msg, max_iters, context_tokens);
        if (!ctx) {
            MIMI_LOGW(TAG, "No free agent slot; dropping message from %s:%s",
                      msg.channel, msg.chat_id);
            free(msg.content);
            continue;
        }

        agent_process_user_turn(ctx,
                                 &msg,
                                 memory_window,
                                 context_tokens,
                                 flush_threshold_ratio,
                                 compaction_model);

        free(msg.content);
    }

}

/* ==========================================================================
 * Tool Confirmation Functions
 * ========================================================================== */

/**
 * Callback for tool confirmation response
 */
static void tool_confirm_callback(const char *request_id, const char *response, void *context)
{
    tool_call_context_t *tool_ctx = (tool_call_context_t *)context;
    if (!tool_ctx) {
        MIMI_LOGW(TAG, "Tool confirm callback with null context");
        return;
    }
    
    MIMI_LOGI(TAG, "Tool confirmation response: id=%s, response=%s", request_id, response);
    
    /* Parse confirmation result */
    if (strcmp(response, "ACCEPT") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_ACCEPTED;
        /* Keep waiting_for_confirmation true until execution completes. */
        
        /* Execute the tool */
        MIMI_LOGI(TAG, "Tool execution confirmed: %s", tool_ctx->tool_name);
        agent_send_status(tool_ctx->session_ctx.channel, tool_ctx->session_ctx.chat_id,
                          MIMI_STATUS_PHASE_PROGRESS, "agent_turn",
                          "▶️ **已授权，开始执行工具…**");
        
        /* Execute tool asynchronously */
        mimi_err_t err = tool_registry_execute_async(
            tool_ctx->tool_name,
            tool_ctx->tool_input,
            &tool_ctx->session_ctx,
            tool_confirm_execution_callback,
            tool_ctx
        );
        
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to execute tool %s: %s", tool_ctx->tool_name, mimi_err_to_name(err));
            tool_ctx->error_code = err;
            tool_ctx->succeeded = false;
            tool_ctx->waiting_for_confirmation = false;
            agent_request_finish((agent_request_ctx_t *)tool_ctx->agent_ctx);
            tool_call_context_destroy(tool_ctx);
        } else {
            tool_ctx->executed = true;
        }
        
    } else if (strcmp(response, "ACCEPT_ALWAYS") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_ACCEPTED_ALWAYS;
        /* Keep waiting_for_confirmation true until execution completes. */
        
        /* Mark tool as always allowed */
        tool_call_context_mark_always_allowed(tool_ctx->tool_name);
        
        /* Execute the tool */
        MIMI_LOGI(TAG, "Tool execution confirmed (always): %s", tool_ctx->tool_name);
        agent_send_status(tool_ctx->session_ctx.channel, tool_ctx->session_ctx.chat_id,
                          MIMI_STATUS_PHASE_PROGRESS, "agent_turn",
                          "▶️ **已永久授权，开始执行工具…**");
        
        /* Execute tool asynchronously */
        mimi_err_t err = tool_registry_execute_async(
            tool_ctx->tool_name,
            tool_ctx->tool_input,
            &tool_ctx->session_ctx,
            tool_confirm_execution_callback,
            tool_ctx
        );
        
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to execute tool %s: %s", tool_ctx->tool_name, mimi_err_to_name(err));
            tool_ctx->error_code = err;
            tool_ctx->succeeded = false;
            tool_ctx->waiting_for_confirmation = false;
            agent_request_finish((agent_request_ctx_t *)tool_ctx->agent_ctx);
            tool_call_context_destroy(tool_ctx);
        } else {
            tool_ctx->executed = true;
        }
        
    } else if (strcmp(response, "REJECT") == 0 || strcmp(response, "CANCELLED") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_REJECTED;
        tool_ctx->waiting_for_confirmation = false;
        
        MIMI_LOGI(TAG, "Tool execution rejected: %s", tool_ctx->tool_name);
        agent_send_status(tool_ctx->session_ctx.channel, tool_ctx->session_ctx.chat_id,
                          MIMI_STATUS_PHASE_PROGRESS, "agent_turn",
                          "⛔ **已拒绝授权：工具不会执行**");
        
        /* Send cancellation message to user */
        if (tool_ctx->agent_ctx) {
            mimi_msg_t cancel_msg = {0};
            strncpy(cancel_msg.channel, tool_ctx->session_ctx.channel, sizeof(cancel_msg.channel) - 1);
            strncpy(cancel_msg.chat_id, tool_ctx->session_ctx.chat_id, sizeof(cancel_msg.chat_id) - 1);
            cancel_msg.content = strdup("Tool execution cancelled by user");
            if (cancel_msg.content) {
                message_bus_push_outbound(&cancel_msg);
            }
        }

        agent_request_finish((agent_request_ctx_t *)tool_ctx->agent_ctx);
        
    } else if (strcmp(response, "TIMEOUT") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_TIMEOUT;
        tool_ctx->waiting_for_confirmation = false;
        
        MIMI_LOGW(TAG, "Tool confirmation timeout: %s", tool_ctx->tool_name);
        agent_send_status(tool_ctx->session_ctx.channel, tool_ctx->session_ctx.chat_id,
                          MIMI_STATUS_PHASE_PROGRESS, "agent_turn",
                          "⌛ **授权超时：操作已取消**");
        
        /* Send timeout message to user */
        if (tool_ctx->agent_ctx) {
            mimi_msg_t timeout_msg = {0};
            strncpy(timeout_msg.channel, tool_ctx->session_ctx.channel, sizeof(timeout_msg.channel) - 1);
            strncpy(timeout_msg.chat_id, tool_ctx->session_ctx.chat_id, sizeof(timeout_msg.chat_id) - 1);
            timeout_msg.content = strdup("Confirmation timeout, operation cancelled");
            if (timeout_msg.content) {
                message_bus_push_outbound(&timeout_msg);
            }
        }

        agent_request_finish((agent_request_ctx_t *)tool_ctx->agent_ctx);
    }
    
    /* Release the reference held for the callback */
    tool_call_context_destroy(tool_ctx);
}

/**
 * Send confirmation request for a tool
 */
static mimi_err_t send_tool_confirmation_request(tool_call_context_t *tool_ctx)
{
    if (!tool_ctx || !tool_ctx->agent_ctx) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    agent_request_ctx_t *agent_ctx = (agent_request_ctx_t *)tool_ctx->agent_ctx;
    
    /* Build confirmation data */
    char data[2048];
    snprintf(data, sizeof(data), "Tool: %.100s\nInput: %.1800s",
             tool_ctx->tool_name, tool_ctx->tool_input);
    
    /* Retain the context for the callback */
    tool_call_context_retain(tool_ctx);
    
    /* Send control request */
    char request_id[64];
    mimi_err_t err = control_manager_send_request(
        agent_ctx->channel,
        agent_ctx->chat_id,
        MIMI_CONTROL_TYPE_CONFIRM,
        tool_ctx->tool_name,
        data,
        tool_ctx,
        tool_confirm_callback,
        request_id
    );
    
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to send confirmation request: %s", mimi_err_to_name(err));
        tool_call_context_destroy(tool_ctx); /* Release the retained reference */
        return err;
    }
    
    MIMI_LOGI(TAG, "Tool confirmation request sent: %s (id=%s)", 
              tool_ctx->tool_name, request_id);
    
    return MIMI_OK;
}

mimi_err_t agent_async_loop_init(void)
{
    mimi_err_t err;
    
    /* Initialize control manager */
    err = control_manager_init();
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to init control manager: %s", mimi_err_to_name(err));
        return err;
    }
    
    /* Initialize tool call context manager */
    err = tool_call_context_manager_init();
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to init tool call context manager: %s", mimi_err_to_name(err));
        control_manager_deinit();
        return err;
    }
    
    MIMI_LOGD(TAG, "Async agent loop initialized");
    return MIMI_OK;
}

mimi_err_t agent_async_loop_start(void)
{
    s_agent_running = true;
    return mimi_task_create_detached("agent_async_loop", agent_async_loop_task, NULL);
}

void agent_async_loop_stop(void)
{
    MIMI_LOGI(TAG, "Async agent loop stopping");
    s_agent_running = false;
    
    /* Deinitialize managers */
    tool_call_context_manager_deinit();
    control_manager_deinit();
}
