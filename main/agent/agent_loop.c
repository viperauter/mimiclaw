#include "agent_loop.h"
#include "agent/context_builder.h"
#include "config.h"
#include "bus/message_bus.h"
#include "channels/channel.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"
#include "os/os.h"
#include "cJSON.h"
#include "fs/fs.h"

static const char *TAG = "agent";

#define CONTEXT_BUF_SIZE     (16 * 1024)
#define LLM_STREAM_BUF_SIZE  (32 * 1024)
#define TOOL_OUTPUT_SIZE     (8 * 1024)

/* Build the assistant content array from llm_response_t for the messages history.
 * Returns a cJSON array with text and tool_use blocks. */
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
        if (!input) {
            input = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(tool_block, "input", input);

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

static void json_set_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) {
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static void append_turn_context_prompt(char *prompt, size_t size, const mimi_msg_t *msg)
{
    if (!prompt || size == 0 || !msg) {
        return;
    }

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) {
        return;
    }

    int n = snprintf(
        prompt + off, size - off,
        "\n## Current Turn Context\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n"
        "- If using cron_add for Telegram in this turn, set channel='telegram' and chat_id to source_chat_id.\n"
        "- Never use chat_id 'cron' for Telegram messages.\n",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call, const mimi_msg_t *msg)
{
    if (!call || !msg || strcmp(call->name, "cron_add") != 0) {
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
    const char *channel = cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

    if ((!channel || channel[0] == '\0') && msg->channel[0] != '\0') {
        json_set_string(root, "channel", msg->channel);
        channel = msg->channel;
        changed = true;
    }

    if (channel && strcmp(channel, MIMI_CHAN_TELEGRAM) == 0 &&
        strcmp(msg->channel, MIMI_CHAN_TELEGRAM) == 0 && msg->chat_id[0] != '\0') {
        cJSON *chat_item = cJSON_GetObjectItem(root, "chat_id");
        const char *chat_id = cJSON_IsString(chat_item) ? chat_item->valuestring : NULL;
        if (!chat_id || chat_id[0] == '\0' || strcmp(chat_id, "cron") == 0) {
            json_set_string(root, "chat_id", msg->chat_id);
            changed = true;
        }
    }

    char *patched = NULL;
    if (changed) {
            patched = cJSON_PrintUnformatted(root);
            if (patched) {
                MIMI_LOGI(TAG, "Patched cron_add target to %s:%s", msg->channel, msg->chat_id);
            }
    }

    cJSON_Delete(root);
    return patched;
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp, const mimi_msg_t *msg,
                                 char *tool_output, size_t tool_output_size)
{
    cJSON *content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = call->input ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, msg);
        if (patched_input) {
            tool_input = patched_input;
        }

        /* Execute tool with session context */
        tool_output[0] = '\0';
        mimi_session_ctx_t session_ctx;
        session_ctx_from_msg(msg, &session_ctx);
        tool_registry_execute(call->name, tool_input, tool_output, tool_output_size, &session_ctx);
        free(patched_input);

        MIMI_LOGI(TAG, "Tool %s result: %d bytes", call->name, (int)strlen(tool_output));

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}

static void agent_loop_task(void *arg)
{
    (void)arg;
    MIMI_LOGI(TAG, "Agent loop started");

    const mimi_config_t *cfg = mimi_config_get();
    int memory_window = (cfg->memory_window > 0) ? cfg->memory_window : 100;
    int max_iters = (cfg->max_tool_iterations > 0) ? cfg->max_tool_iterations : 40;

    char *system_prompt = (char *)calloc(1, CONTEXT_BUF_SIZE);
    char *history_json = (char *)calloc(1, LLM_STREAM_BUF_SIZE);
    char *tool_output = (char *)calloc(1, TOOL_OUTPUT_SIZE);

    if (!system_prompt || !history_json || !tool_output) {
        MIMI_LOGE(TAG, "Failed to allocate buffers");
        free(system_prompt);
        free(history_json);
        free(tool_output);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        mimi_msg_t msg;
        mimi_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != MIMI_OK) continue;

        MIMI_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);

        /* Ensure per-session workspace directory exists:
         *   workspaces/<channel>_<chat_id>/
         * (This is a filesystem layout convention; tool routing can use it later.) */
        if (msg.channel[0] && msg.chat_id[0]) {
            char ws_dir[512];
            snprintf(ws_dir, sizeof(ws_dir), "workspaces/%s_%s", msg.channel, msg.chat_id);
            (void)mimi_fs_mkdir_p(ws_dir);
        }

        /* 1. Build system prompt */
        context_build_system_prompt(system_prompt, CONTEXT_BUF_SIZE);
        append_turn_context_prompt(system_prompt, CONTEXT_BUF_SIZE, &msg);
        MIMI_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel, msg.chat_id);

        /* 2. Load session history into cJSON array */
        session_get_history_json(msg.channel, msg.chat_id, history_json,
                                 LLM_STREAM_BUF_SIZE, memory_window);

        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) messages = cJSON_CreateArray();

        /* 3. Append current user message */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        bool sent_working_status = false;

        while (iteration < max_iters) {
            /* Send "working" indicator before each API call */
            if (cfg->send_working_status && !sent_working_status && strcmp(msg.channel, MIMI_CHAN_SYSTEM) != 0) {
                mimi_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                status.content = strdup("\xF0\x9F\x90\xB1mimi is working...");
                if (status.content) {
                    if (message_bus_push_outbound(&status) != MIMI_OK) {
                        MIMI_LOGW(TAG, "Outbound queue full, drop working status");
                        free(status.content);
                    } else {
                        sent_working_status = true;
                    }
                }
            }

            llm_response_t resp;
            err = llm_chat_tools(system_prompt, messages, tools_json, &resp);

            if (err != MIMI_OK) {
                MIMI_LOGE(TAG, "LLM call failed: %s", mimi_err_to_name(err));
                
                /* Send LLM error message to channel */
                const char *llm_error = llm_get_last_error();
                if (llm_error && llm_error[0]) {
                    char error_msg[1024];
                    snprintf(error_msg, sizeof(error_msg), "LLM Error: %s\n%s", 
                             mimi_err_to_name(err), llm_error);
                    channel_send(msg.channel, msg.chat_id, error_msg);
                } else {
                    char error_msg[512];
                    snprintf(error_msg, sizeof(error_msg), "LLM Error: %s", 
                             mimi_err_to_name(err));
                    channel_send(msg.channel, msg.chat_id, error_msg);
                }
                break;
            }

            if (!resp.tool_use) {
                /* Normal completion — save final text and break */
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                }
                llm_response_free(&resp);
                break;
            }

            MIMI_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);

            /* Append assistant message with content array */
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Execute tools and append results */
            cJSON *tool_results = build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE);
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            llm_response_free(&resp);
            iteration++;
        }

        cJSON_Delete(messages);

        /* 5. Send response */
        if (final_text && final_text[0]) {
            /* Save to session (only user text + final assistant text) */
            mimi_err_t save_user = session_append(msg.channel, msg.chat_id, "user", msg.content);
            mimi_err_t save_asst = session_append(msg.channel, msg.chat_id, "assistant", final_text);
            if (save_user != MIMI_OK || save_asst != MIMI_OK) {
                MIMI_LOGW(TAG, "Session save failed for chat %s (user=%s, assistant=%s)",
                         msg.chat_id,
                         mimi_err_to_name(save_user),
                         mimi_err_to_name(save_asst));
            } else {
                MIMI_LOGI(TAG, "Session saved for chat %s", msg.chat_id);
            }

            /* Push response to outbound */
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = final_text;  /* transfer ownership */
            MIMI_LOGI(TAG, "Queue final response to %s:%s (%d bytes)",
                     out.channel, out.chat_id, (int)strlen(final_text));
            if (message_bus_push_outbound(&out) != MIMI_OK) {
                MIMI_LOGW(TAG, "Outbound queue full, drop final response");
                free(final_text);
            } else {
                final_text = NULL;
            }
        } else {
            /* Error or empty response */
            free(final_text);
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup("Sorry, I encountered an error.");
            if (out.content) {
                if (message_bus_push_outbound(&out) != MIMI_OK) {
                    MIMI_LOGW(TAG, "Outbound queue full, drop error response");
                    free(out.content);
                }
            }
        }

        /* Free inbound message content */
        free(msg.content);

        /* Log memory status */
        /* no-op on POSIX */
    }
}

mimi_err_t agent_loop_init(void)
{
    MIMI_LOGI(TAG, "Agent loop initialized");
    return MIMI_OK;
}

mimi_err_t agent_loop_start(void)
{
    return mimi_task_create_detached("agent_loop", agent_loop_task, NULL);
}
