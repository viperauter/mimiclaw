/**
 * Help Command Implementation
 *
 * Provides help information for all registered commands
 */

#include "commands/command.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cmd_help";

/**
 * Execute help command
 * Usage: /help [command_name]
 */
static int cmd_help_execute(const char **args, int arg_count,
                            const command_context_t *ctx,
                            char *output, size_t output_len)
{
    (void)ctx;

    if (arg_count == 0) {
        /* Show all commands */
        command_get_help(output, output_len);
        return 0;
    }

    /* Show help for specific command */
    command_get_help_for(args[0], output, output_len);
    return 0;
}

/* Command definition */
static const command_t cmd_help = {
    .name = "help",
    .description = "Show help information for commands",
    .usage = "/help [command_name]",
    .execute = cmd_help_execute,
};

/**
 * Initialize help command
 * Registers the command with the command system
 */
void cmd_help_init(void)
{
    int ret = command_register(&cmd_help);
    if (ret != 0) {
        MIMI_LOGW(TAG, "Failed to register help command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Help command registered");
    }
}
