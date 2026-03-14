/**
 * Set Command Implementation
 *
 * Unified configuration command:
 *   /set api_key <KEY>
 *   /set model <MODEL>
 *   /set model_provider <anthropic|openai|openrouter>
 *   /set tg_token <TOKEN>
 *   /set search_key <BRAVE_KEY>
 */

#include "commands/command.h"
#include "llm/llm_proxy.h"
#include "channels/telegram/telegram_channel.h"
#include "tools/tool_web_search.h"
#include "bus/message_bus.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cmd_set";

/**
 * Execute set command
 */
static int cmd_set_execute(const char **args, int arg_count,
                           const command_context_t *ctx,
                           char *output, size_t output_len)
{
    (void)ctx;

    if (arg_count < 2) {
        snprintf(output, output_len,
                 "Usage: /set <api_key|model|model_provider|tg_token|search_key> <value>\n"
                 "  api_key        - Set LLM API key\n"
                 "  model          - Set model name\n"
                 "  model_provider - Set provider (anthropic|openai|openrouter)\n"
                 "  tg_token       - Set Telegram bot token\n"
                 "  search_key     - Set Brave search API key");
        return -1;
    }

    const char *sub = args[0];
    const char *value = args[1];
    mimi_err_t err = MIMI_ERR_INVALID_ARG;
    const char *setting_name = NULL;

    if (strcmp(sub, "api_key") == 0) {
        err = llm_set_api_key(value);
        setting_name = "API key";
    } else if (strcmp(sub, "model") == 0) {
        err = llm_set_model(value);
        setting_name = "Model";
    } else if (strcmp(sub, "model_provider") == 0) {
        err = llm_set_provider(value);
        setting_name = "Model provider";
    } else if (strcmp(sub, "tg_token") == 0) {
        err = telegram_channel_set_token(value);
        setting_name = "Telegram token";
    } else if (strcmp(sub, "search_key") == 0) {
        err = tool_web_search_set_key(value);
        setting_name = "Search key";
    } else {
        snprintf(output, output_len,
                 "Unknown setting: %s\n"
                 "Use: api_key, model, model_provider, tg_token, search_key", sub);
        return -1;
    }

    if (err == MIMI_OK) {
        snprintf(output, output_len, "OK - %s set successfully", setting_name);
    } else {
        snprintf(output, output_len, "Error: %s", mimi_err_to_name(err));
    }

    return (err == MIMI_OK) ? 0 : -1;
}

/* Command definition */
static const command_t cmd_set = {
    .name = "set",
    .description = "Configure settings (api_key, model, tg_token, etc.)",
    .usage = "/set <api_key|model|model_provider|tg_token|search_key> <value>",
    .execute = cmd_set_execute,
};

/**
 * Initialize set command
 */
void cmd_set_init(void)
{
    int ret = command_register(&cmd_set);
    if (ret != 0) {
        MIMI_LOGW(TAG, "Failed to register set command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Set command registered");
    }
}
