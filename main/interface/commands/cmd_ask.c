/**
 * Ask Command Implementation
 *
 * Inject a message into the agent for processing:
 *   /ask <channel> <chat_id> <text...>
 */

#include "commands/command.h"
#include "bus/message_bus.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "cmd_ask";

/**
 * Execute ask command
 */
static int cmd_ask_execute(const char **args, int arg_count,
                           const command_context_t *ctx,
                           char *output, size_t output_len)
{
    (void)ctx;

    if (arg_count < 3) {
        snprintf(output, output_len,
                 "Usage: /ask <channel> <chat_id> <text...>\n"
                 "Inject a message into the agent for processing.");
        return -1;
    }

    const char *channel = args[0];
    const char *chat_id = args[1];

    /* Join remaining arguments into a single string */
    size_t text_len = 0;
    for (int i = 2; i < arg_count; i++) {
        text_len += strlen(args[i]) + 1;  /* +1 for space or null */
    }

    char *text = (char *)calloc(1, text_len);
    if (!text) {
        snprintf(output, output_len, "Error: Out of memory");
        return -1;
    }

    for (int i = 2; i < arg_count; i++) {
        strcat(text, args[i]);
        if (i != arg_count - 1) {
            strcat(text, " ");
        }
    }

    /* Create and push message to bus */
    mimi_msg_t msg = {0};
    strncpy(msg.channel, channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    msg.content = text;  /* Transfer ownership */

    mimi_err_t err = message_bus_push_inbound(&msg);

    if (err == MIMI_OK) {
        snprintf(output, output_len,
                 "Message injected into agent [channel=%s, chat_id=%s]",
                 channel, chat_id);
    } else {
        snprintf(output, output_len, "Error: %s", mimi_err_to_name(err));
    }

    /* Note: text ownership transferred to message bus on success,
     * otherwise we need to free it */
    if (err != MIMI_OK) {
        free(text);
    }

    return (err == MIMI_OK) ? 0 : -1;
}

/* Command definition */
static const command_t cmd_ask = {
    .name = "ask",
    .description = "Inject a message into the agent",
    .usage = "/ask <channel> <chat_id> <text...>",
    .execute = cmd_ask_execute,
};

/**
 * Initialize ask command
 */
void cmd_ask_init(void)
{
    int ret = command_register(&cmd_ask);
    if (ret != 0) {
        MIMI_LOGW(TAG, "Failed to register ask command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Ask command registered");
    }
}
