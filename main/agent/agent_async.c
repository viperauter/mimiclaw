#include "agent_async.h"
#include "agent/context_builder.h"
#include "config.h"
#include "bus/message_bus.h"
#include "channels/channel.h"
#include "llm/llm_proxy.h"
#include "llm/llm_trace.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"
#include "os/os.h"
#include "cJSON.h"
#include "fs/fs.h"

static const char *TAG = "agent_async";

/* Build the assistant content array from llm_response_t for the messages history. */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp && resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    if (!resp) {
        return content;
    }

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id[0] ? call->id : "");
        cJSON_AddStringToObject(tool_block, "name", call->name[0] ? call->name : "");

        const char *input_json = (call->input && call->input[0]) ? call->input : "{}";
        cJSON *input = cJSON_Parse(input_json);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddObjectToObject(tool_block, "input");
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

/* Build tool results array from llm_response_t */
static cJSON *build_tool_results(const llm_response_t *resp,
                                 const void *msg_ptr,
                                 char *output_buf,
                                 size_t output_buf_size,
                                 const agent_async_ctx_t *ctx)
{
    if (!resp || resp->call_count == 0) {
        cJSON *empty = cJSON_CreateArray();
        return empty;
    }

    const mimi_msg_t *msg = (const mimi_msg_t *)msg_ptr;
    cJSON *results = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "tool_call_id", call->id[0] ? call->id : "");
        cJSON_AddStringToObject(result, "tool_name", call->name[0] ? call->name : "");

        /* Execute tool */
        mimi_session_ctx_t session_ctx;
        session_ctx_from_msg((const mimi_msg_t *)msg, &session_ctx);
        if (ctx && ctx->trace_id[0]) {
            char itbuf[32];
            snprintf(itbuf, sizeof(itbuf), "%d", ctx->iteration + 1);
            llm_trace_event_kv(ctx->trace_id, "tool_start",
                               "tool_name", call->name[0] ? call->name : "",
                               "tool_call_id", call->id[0] ? call->id : "",
                               "input", (call->input && call->input[0]) ? call->input : "{}",
                               "iteration", itbuf);
        }

        mimi_err_t te = tool_registry_execute(call->name, call->input, output_buf, output_buf_size, &session_ctx);

        if (ctx && ctx->trace_id[0]) {
            llm_trace_event_kv(ctx->trace_id, "tool_result",
                               "tool_name", call->name[0] ? call->name : "",
                               "result", mimi_err_to_name(te),
                               "output", output_buf[0] ? output_buf : "{}",
                               NULL, NULL);
        }

        cJSON_AddStringToObject(result, "output", output_buf[0] ? output_buf : "{}");
        cJSON_AddItemToArray(results, result);
    }

    return results;
}

/* LLM callback function */
static void agent_llm_callback(mimi_err_t result, llm_response_t *resp, void *user_data)
{
    agent_async_ctx_t *ctx = (agent_async_ctx_t *)user_data;
    
    if (result != MIMI_OK) {
        MIMI_LOGE(TAG, "LLM call failed: %s", mimi_err_to_name(result));
        
        /* Send LLM error message to channel */
        const char *llm_error = llm_get_last_error();
        if (llm_error && llm_error[0]) {
            char error_msg[1024];
            snprintf(error_msg, sizeof(error_msg), "LLM Error: %s\n%s", 
                     mimi_err_to_name(result), llm_error);
            mimi_msg_t msg = {0};
            strncpy(msg.channel, ctx->channel, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, ctx->chat_id, sizeof(msg.chat_id) - 1);
            msg.type = MIMI_MSG_TYPE_TEXT;
            msg.content = strdup(error_msg);
            if (msg.content) {
                channel_send(&msg);
                free(msg.content);
            }
        } else {
            char error_msg[512];
            snprintf(error_msg, sizeof(error_msg), "LLM Error: %s", 
                     mimi_err_to_name(result));
            mimi_msg_t msg = {0};
            strncpy(msg.channel, ctx->channel, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, ctx->chat_id, sizeof(msg.chat_id) - 1);
            msg.type = MIMI_MSG_TYPE_TEXT;
            msg.content = strdup(error_msg);
            if (msg.content) {
                channel_send(&msg);
                free(msg.content);
            }
        }
        
        /* Call completion callback */
        if (ctx->completion_callback) {
            ctx->completion_callback(result, NULL, ctx->user_data);
        }
        
        return;
    }

    if (!resp->tool_use) {
        /* Normal completion — save final text */
        if (resp->text && resp->text_len > 0) {
            strncpy(ctx->final_text, resp->text, sizeof(ctx->final_text) - 1);
        }
        
        /* Send final response */
        if (ctx->final_text[0]) {
            mimi_msg_t msg = {0};
            strncpy(msg.channel, ctx->channel, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, ctx->chat_id, sizeof(msg.chat_id) - 1);
            msg.type = MIMI_MSG_TYPE_TEXT;
            msg.content = strdup(ctx->final_text);
            if (msg.content) {
                channel_send(&msg);
                free(msg.content);
            }
        }
        
        /* Call completion callback */
        if (ctx->completion_callback) {
            ctx->completion_callback(MIMI_OK, ctx->final_text, ctx->user_data);
        }
        
        llm_response_free(resp);
        return;
    }

    MIMI_LOGI(TAG, "Tool use iteration %d: %d calls", ctx->iteration + 1, resp->call_count);

    /* Append assistant message with content array */
    cJSON *asst_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(asst_msg, "role", "assistant");
    cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(resp));
    cJSON_AddItemToArray(ctx->messages, asst_msg);

    /* Execute tools and append results */
    mimi_msg_t msg = {0};
    strncpy(msg.channel, ctx->channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, ctx->chat_id, sizeof(msg.chat_id) - 1);
    cJSON *tool_results = build_tool_results(resp, &msg, ctx->tool_output, sizeof(ctx->tool_output), ctx);
    
    /* Append tool results */
    cJSON *tool_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_msg, "role", "tool");
    cJSON_AddItemToObject(tool_msg, "content", tool_results);
    cJSON_AddItemToArray(ctx->messages, tool_msg);

    /* Increment iteration and continue */
    ctx->iteration++;
    
    if (ctx->iteration >= ctx->max_iters) {
        /* Max iterations reached */
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg), "Max iterations reached (%d)", ctx->max_iters);
        mimi_msg_t msg = {0};
        strncpy(msg.channel, ctx->channel, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, ctx->chat_id, sizeof(msg.chat_id) - 1);
        msg.type = MIMI_MSG_TYPE_TEXT;
        msg.content = strdup(error_msg);
        if (msg.content) {
            channel_send(&msg);
            free(msg.content);
        }
        
        if (ctx->completion_callback) {
            ctx->completion_callback(MIMI_ERR_TIMEOUT, NULL, ctx->user_data);
        }
        
        llm_response_free(resp);
        return;
    }

    /* Continue with next LLM call */
    llm_response_t *next_resp = (llm_response_t *)malloc(sizeof(llm_response_t));
    if (!next_resp) {
        MIMI_LOGE(TAG, "Failed to allocate LLM response");
        if (ctx->completion_callback) {
            ctx->completion_callback(MIMI_ERR_NO_MEM, NULL, ctx->user_data);
        }
        llm_response_free(resp);
        return;
    }
    memset(next_resp, 0, sizeof(llm_response_t));
    
    llm_chat_req_t req = {
        .system_prompt = ctx->system_prompt,
        .messages = ctx->messages,
        .tools_json = ctx->tools_json,
        .trace_id = ctx->trace_id[0] ? ctx->trace_id : NULL,
    };
    if (ctx->trace_id[0]) {
        char itbuf[32];
        snprintf(itbuf, sizeof(itbuf), "%d", ctx->iteration + 1);
        llm_trace_event_kv(ctx->trace_id, "llm_call_start",
                           "iteration", itbuf,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    }
    mimi_err_t err = llm_chat_tools_async_req(&req, next_resp, agent_llm_callback, ctx);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start LLM async: %s", mimi_err_to_name(err));
        if (ctx->completion_callback) {
            ctx->completion_callback(err, NULL, ctx->user_data);
        }
        free(next_resp);
    }
    
    llm_response_free(resp);
}

mimi_err_t agent_async_init(agent_async_ctx_t *ctx, const char *channel, const char *chat_id, const char *content, void (*completion_cb)(mimi_err_t, const char *, void *), void *user_data)
{
    if (!ctx || !channel || !chat_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    memset(ctx, 0, sizeof(*ctx));
    
    /* Copy channel and chat_id */
    strncpy(ctx->channel, channel, sizeof(ctx->channel) - 1);
    strncpy(ctx->chat_id, chat_id, sizeof(ctx->chat_id) - 1);
    strncpy(ctx->content, content, sizeof(ctx->content) - 1);
    
    /* Set callback and user data */
    ctx->completion_callback = completion_cb;
    ctx->user_data = user_data;
    
    /* Set default values */
    ctx->max_iters = 5;
    ctx->iteration = 0;
    ctx->sent_working_status = false;
    
    return MIMI_OK;
}

mimi_err_t agent_async_start(agent_async_ctx_t *ctx)
{
    if (!ctx) {
        return MIMI_ERR_INVALID_ARG;
    }

    llm_trace_make_id(ctx->trace_id, sizeof(ctx->trace_id));
    llm_trace_bind_session(ctx->trace_id, ctx->channel, ctx->chat_id);
    llm_trace_event_kv(ctx->trace_id, "request_start",
                       "channel", ctx->channel,
                       "chat_id", ctx->chat_id,
                       "user_input", ctx->content,
                       NULL, NULL);
    
    /* Build system prompt */
    context_build_system_prompt(ctx->system_prompt, CONTEXT_BUF_SIZE);
    if (ctx->trace_id[0]) {
        llm_trace_event_kv(ctx->trace_id, "system_prompt",
                           "prompt", ctx->system_prompt,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    }
    
    /* Ensure per-session workspace directory exists */
    if (ctx->channel[0] && ctx->chat_id[0]) {
        char ws_dir[512];
        snprintf(ws_dir, sizeof(ws_dir), "workspaces/%s_%s", ctx->channel, ctx->chat_id);
        (void)mimi_fs_mkdir_p(ws_dir);
    }
    
    /* Load session history */
    char history_json[LLM_STREAM_BUF_SIZE] = {0};
    int memory_window = 20;
    session_get_history_json(ctx->channel, ctx->chat_id, history_json, LLM_STREAM_BUF_SIZE, memory_window);
    
    /* Parse or create messages array */
    ctx->messages = cJSON_Parse(history_json);
    if (!ctx->messages) {
        ctx->messages = cJSON_CreateArray();
    }
    
    /* Append current user message */
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", ctx->content);
    cJSON_AddItemToArray(ctx->messages, user_msg);
    
    /* Get tools JSON */
    ctx->tools_json = tool_registry_get_tools_json();
    if (ctx->trace_id[0]) {
        llm_trace_event_kv(ctx->trace_id, "tools",
                           "tools_json", ctx->tools_json ? ctx->tools_json : "",
                           NULL, NULL, NULL, NULL, NULL, NULL);
        char *messages_json = cJSON_PrintUnformatted(ctx->messages);
        if (messages_json) {
            llm_trace_event_json(ctx->trace_id, "messages", messages_json);
            free(messages_json);
        }
        llm_trace_event_kv(ctx->trace_id, "llm_call_start",
                           "iteration", "1",
                           NULL, NULL, NULL, NULL, NULL, NULL);
    }
    
    /* Send working status */
    if (strcmp(ctx->channel, MIMI_CHAN_SYSTEM) != 0) {
        mimi_msg_t status = {0};
        strncpy(status.channel, ctx->channel, sizeof(status.channel) - 1);
        strncpy(status.chat_id, ctx->chat_id, sizeof(status.chat_id) - 1);
        status.content = strdup("\xF0\x9F\x90\xB1mimi is working...");
        if (status.content) {
            if (message_bus_push_outbound(&status) != MIMI_OK) {
                MIMI_LOGW(TAG, "Outbound queue full, drop working status");
                free(status.content);
            } else {
                ctx->sent_working_status = true;
            }
        }
    }
    
    /* Start first LLM call */
    llm_response_t *resp = (llm_response_t *)malloc(sizeof(llm_response_t));
    if (!resp) {
        return MIMI_ERR_NO_MEM;
    }
    memset(resp, 0, sizeof(*resp));
    
    llm_chat_req_t req = {
        .system_prompt = ctx->system_prompt,
        .messages = ctx->messages,
        .tools_json = ctx->tools_json,
        .trace_id = ctx->trace_id[0] ? ctx->trace_id : NULL,
    };
    mimi_err_t err = llm_chat_tools_async_req(&req, resp, agent_llm_callback, ctx);
    if (err != MIMI_OK) {
        free(resp);
        return err;
    }
    
    return MIMI_OK;
}

void agent_async_cleanup(agent_async_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    
    /* Free messages */
    if (ctx->messages) {
        cJSON_Delete(ctx->messages);
        ctx->messages = NULL;
    }
    
    /* Clear buffers */
    memset(ctx->system_prompt, 0, sizeof(ctx->system_prompt));
    memset(ctx->channel, 0, sizeof(ctx->channel));
    memset(ctx->chat_id, 0, sizeof(ctx->chat_id));
    memset(ctx->trace_id, 0, sizeof(ctx->trace_id));
    memset(ctx->content, 0, sizeof(ctx->content));
    memset(ctx->final_text, 0, sizeof(ctx->final_text));
    memset(ctx->tool_output, 0, sizeof(ctx->tool_output));
    
    /* Reset state */
    ctx->iteration = 0;
    ctx->max_iters = 5;
    ctx->sent_working_status = false;
    ctx->tools_json = NULL;
    ctx->completion_callback = NULL;
    ctx->user_data = NULL;
}
