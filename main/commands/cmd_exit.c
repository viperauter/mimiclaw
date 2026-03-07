/**
 * Exit Command Implementation
 *
 * Exit the application:
 *   /exit
 */

#include "commands/command.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cmd_exit";

/**
 * Execute exit command
 */
static int cmd_exit_execute(const char **args, int arg_count,
                            const command_context_t *ctx,
                            char *output, size_t output_len)
{
    (void)args;
    (void)arg_count;
    (void)ctx;

    snprintf(output, output_len, "Goodbye!");

    /* Return special exit code to signal application exit */
    return -100;  /* Special exit code */
}

/* Command definition */
static const command_t cmd_exit = {
    .name = "exit",
    .description = "Exit the application",
    .usage = "/exit",
    .execute = cmd_exit_execute,
};

/**
 * Initialize exit command
 */
void cmd_exit_init(void)
{
    int ret = command_register(&cmd_exit);
    if (ret != 0) {
        MIMI_LOGW(TAG, "Failed to register exit command: %d", ret);
    } else {
        MIMI_LOGI(TAG, "Exit command registered");
    }
}
