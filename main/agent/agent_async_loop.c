#include "agent_async_loop.h"
#include "agent/context_builder.h"
#include "config.h"
#include "bus/message_bus.h"
#include "channels/channel.h"
#include "llm/llm_proxy.h"
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

static void agent_llm_callback(mimi_err_t result, llm_response_t *resp, void *user_data);

#define CONTEXT_BUF_SIZE     (16 * 1024)
#define LLM_STREAM_BUF_SIZE  (32 * 1024)
#ifndef MAX_CONCURRENT
#define MAX_CONCURRENT       8
#endif

typedef struct {
    char channel[64];
    char chat_id[128];
    char content[32768];
    char *system_prompt;
    cJSON *messages;
    const char *tools_json;
    int iteration;
    int max_iters;
    bool sent_working_status;
    char tool_output[TOOL_OUTPUT_SIZE];
    llm_response_t llm_resp;
    bool in_progress;
} agent_request_ctx_t;

static mimi_mutex_t *s_ctx_mutex = NULL;
static agent_request_ctx_t s_pending_ctx[MAX_CONCURRENT];
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

        mimi_err_t err = llm_chat_tools_async(async_ctx->ctx->system_prompt,
                                              async_ctx->ctx->messages,
                                              async_ctx->ctx->tools_json,
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
    
    /* Store execution results */
    tool_ctx->succeeded = (result == MIMI_OK);
    tool_ctx->error_code = result;
    if (output) {
        strncpy(tool_ctx->output, output, sizeof(tool_ctx->output) - 1);
        tool_ctx->output[sizeof(tool_ctx->output) - 1] = '\0';
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
            message_bus_push_outbound(&result_msg);
        }
    }
    
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
    
    tool_async_ctx_t *async_ctx = (tool_async_ctx_t *)malloc(sizeof(tool_async_ctx_t));
    if (!async_ctx) {
        MIMI_LOGE(TAG, "Failed to allocate async tool context");
        return;
    }
    
    async_ctx->ctx = ctx;
    async_ctx->total_calls = resp->call_count;
    async_ctx->completed_calls = 0;
    async_ctx->mutex = NULL;
    async_ctx->next_llm_started = false;
    
    mimi_err_t err = mimi_mutex_create(&async_ctx->mutex);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create mutex: %s", mimi_err_to_name(err));
        free(async_ctx);
        return;
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
        /* All tools require confirmation, so we need to free async_ctx now
         * because the confirmation process will handle tool execution */
        if (async_ctx->mutex) {
            mimi_mutex_destroy(async_ctx->mutex);
        }
        free(async_ctx);
    }
}

static void agent_llm_callback(mimi_err_t result, llm_response_t *resp, void *user_data);

static void send_working_status(const char *channel, const char *chat_id, bool *sent)
{
    if (*sent || strcmp(channel, MIMI_CHAN_SYSTEM) == 0) {
        return;
    }

    mimi_msg_t status = {0};
    strncpy(status.channel, channel, sizeof(status.channel) - 1);
    strncpy(status.chat_id, chat_id, sizeof(status.chat_id) - 1);
    status.content = strdup("\xF0\x9F\x90\xB1mimi is working...");
    if (status.content) {
        if (message_bus_push_outbound(&status) != MIMI_OK) {
            MIMI_LOGW(TAG, "Outbound queue full, drop working status");
            free(status.content);
        } else {
            *sent = true;
        }
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

    start_async_tools(ctx, resp);
    llm_response_free(resp);

    return;
}

static void send_response(agent_request_ctx_t *ctx, const char *text)
{
    if (!text || !text[0]) {
        mimi_msg_t out = {0};
        strncpy(out.channel, ctx->channel, sizeof(out.channel) - 1);
        strncpy(out.chat_id, ctx->chat_id, sizeof(out.chat_id) - 1);
        out.content = strdup("Sorry, I encountered an error.");
        if (out.content && message_bus_push_outbound(&out) != MIMI_OK) {
            free(out.content);
        }
        goto save;
    }

    mimi_err_t save_user = session_append(ctx->channel, ctx->chat_id, "user", ctx->content);
    mimi_err_t save_asst = session_append(ctx->channel, ctx->chat_id, "assistant", text);
    if (save_user != MIMI_OK || save_asst != MIMI_OK) {
        MIMI_LOGW(TAG, "Session save failed for chat %s", ctx->chat_id);
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
    mimi_err_t save_user2 = session_append(ctx->channel, ctx->chat_id, "user", ctx->content);
    mimi_err_t save_asst2 = session_append(ctx->channel, ctx->chat_id, "assistant", text ? text : "error");
    (void)save_user2;
    (void)save_asst2;
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
        channel_send(ctx->channel, ctx->chat_id, error_msg);
        goto cleanup;
    }

    if (!resp->tool_use) {
        if (resp->text && resp->text_len > 0) {
            send_response(ctx, resp->text);
        }
        llm_response_free(resp);
        goto cleanup;
    }

    MIMI_LOGI(TAG, "Tool use iteration %d: %d calls", ctx->iteration + 1, resp->call_count);
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
    ctx->in_progress = false;
    if (s_ctx_mutex) {
        mimi_mutex_lock(s_ctx_mutex);
    }
    s_active_count--;
    if (s_ctx_mutex) {
        mimi_mutex_unlock(s_ctx_mutex);
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

    const mimi_config_t *cfg = mimi_config_get();
    int memory_window = (cfg->memory_window > 0) ? cfg->memory_window : 100;
    int max_iters = (cfg->max_tool_iterations > 0) ? cfg->max_tool_iterations : 40;

    char *system_prompt = (char *)calloc(1, CONTEXT_BUF_SIZE);
    char *history_json = (char *)calloc(1, LLM_STREAM_BUF_SIZE);

    if (!system_prompt || !history_json) {
        MIMI_LOGE(TAG, "Failed to allocate buffers");
        free(system_prompt);
        free(history_json);
        return;
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

        if (s_ctx_mutex) {
            mimi_mutex_lock(s_ctx_mutex);
        }
        if (s_active_count >= MAX_CONCURRENT) {
            if (s_ctx_mutex) {
                mimi_mutex_unlock(s_ctx_mutex);
            }
            MIMI_LOGW(TAG, "Too many concurrent requests, dropping message from %s:%s", 
                      msg.channel, msg.chat_id);
            free(msg.content);
            continue;
        }

        int slot = -1;
        for (int i = 0; i < MAX_CONCURRENT; i++) {
            if (!s_pending_ctx[i].in_progress) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            if (s_ctx_mutex) {
                mimi_mutex_unlock(s_ctx_mutex);
            }
            free(msg.content);
            continue;
        }

        agent_request_ctx_t *ctx = &s_pending_ctx[slot];
        memset(ctx, 0, sizeof(*ctx));
        strncpy(ctx->channel, msg.channel, sizeof(ctx->channel) - 1);
        strncpy(ctx->chat_id, msg.chat_id, sizeof(ctx->chat_id) - 1);
        strncpy(ctx->content, msg.content, sizeof(ctx->content) - 1);
        ctx->system_prompt = system_prompt;
        ctx->max_iters = max_iters;
        ctx->iteration = 0;
        ctx->in_progress = true;
        s_active_count++;
        if (s_ctx_mutex) {
            mimi_mutex_unlock(s_ctx_mutex);
        }

        MIMI_LOGD(TAG, "Processing message from %s:%s", ctx->channel, ctx->chat_id);
        
        context_build_system_prompt(ctx->system_prompt, CONTEXT_BUF_SIZE);
        char turn_context[512];
        snprintf(turn_context, sizeof(turn_context),
                 "\n## Current Turn Context\n- source_channel: %s\n- source_chat_id: %s\n",
                 msg.channel[0] ? msg.channel : "(unknown)", 
                 msg.chat_id[0] ? msg.chat_id : "(empty)");
        strncat(ctx->system_prompt, turn_context, CONTEXT_BUF_SIZE - strlen(ctx->system_prompt) - 1);

        session_get_history_json(ctx->channel, ctx->chat_id, history_json, LLM_STREAM_BUF_SIZE, memory_window);
        ctx->messages = cJSON_Parse(history_json);
        if (!ctx->messages) {
            ctx->messages = cJSON_CreateArray();
        }

        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(ctx->messages, user_msg);

        ctx->tools_json = tool_registry_get_tools_json();

        send_working_status(ctx->channel, ctx->chat_id, &ctx->sent_working_status);

        llm_response_t *resp = &ctx->llm_resp;
        memset(resp, 0, sizeof(*resp));

        // Print LLM request context for debugging
        char *messages_json = cJSON_PrintUnformatted(ctx->messages);
        if (messages_json) {
            MIMI_LOGD(TAG, "LLM request messages: %s", messages_json);
            free(messages_json);
        }
        
        err = llm_chat_tools_async(ctx->system_prompt, ctx->messages, ctx->tools_json, resp, agent_llm_callback, ctx);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to start async LLM: %s", mimi_err_to_name(err));
            ctx->in_progress = false;
            if (s_ctx_mutex) {
                mimi_mutex_lock(s_ctx_mutex);
            }
            s_active_count--;
            if (s_ctx_mutex) {
                mimi_mutex_unlock(s_ctx_mutex);
            }
            channel_send(ctx->channel, ctx->chat_id, "Failed to start LLM request");
        }

        free(msg.content);
    }

    /* Give in-flight requests a chance to finish before releasing shared buffers. */
    uint64_t start_ms = mimi_time_ms();
    while (mimi_time_ms() - start_ms < 3000) {
        int active = 0;
        if (s_ctx_mutex) mimi_mutex_lock(s_ctx_mutex);
        active = s_active_count;
        if (s_ctx_mutex) mimi_mutex_unlock(s_ctx_mutex);
        if (active <= 0) break;
        mimi_sleep_ms(50);
    }

    free(system_prompt);
    free(history_json);
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
        tool_ctx->waiting_for_confirmation = false;
        
        /* Execute the tool */
        MIMI_LOGI(TAG, "Tool execution confirmed: %s", tool_ctx->tool_name);
        
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
            tool_call_context_destroy(tool_ctx);
        } else {
            tool_ctx->executed = true;
        }
        
    } else if (strcmp(response, "ACCEPT_ALWAYS") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_ACCEPTED_ALWAYS;
        tool_ctx->waiting_for_confirmation = false;
        
        /* Mark tool as always allowed */
        tool_call_context_mark_always_allowed(tool_ctx->tool_name);
        
        /* Execute the tool */
        MIMI_LOGI(TAG, "Tool execution confirmed (always): %s", tool_ctx->tool_name);
        
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
            tool_call_context_destroy(tool_ctx);
        } else {
            tool_ctx->executed = true;
        }
        
    } else if (strcmp(response, "REJECT") == 0 || strcmp(response, "CANCELLED") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_REJECTED;
        tool_ctx->waiting_for_confirmation = false;
        
        MIMI_LOGI(TAG, "Tool execution rejected: %s", tool_ctx->tool_name);
        
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
        
    } else if (strcmp(response, "TIMEOUT") == 0) {
        tool_ctx->confirmation_result = CONFIRMATION_TIMEOUT;
        tool_ctx->waiting_for_confirmation = false;
        
        MIMI_LOGW(TAG, "Tool confirmation timeout: %s", tool_ctx->tool_name);
        
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
