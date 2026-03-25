/**
 * MCP Refresh Command
 *
 * Trigger background MCP discovery/refresh and rebuild tool registry JSON.
 * Usage:
 *   /mcp_refresh [max_attempts] [delay_ms]
 */

#include "interface/commands/command.h"
#include "log.h"
#include "tools/providers/mcp_provider.h"

#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "cmd_mcp_refresh";

static int cmd_mcp_refresh_execute(const char **args, int arg_count,
                                   const command_context_t *ctx,
                                   char *output, size_t output_len)
{
    (void)ctx;

    int max_attempts = 4;
    int delay_ms = 500;

    if (arg_count >= 1 && args[0] && args[0][0]) {
        max_attempts = atoi(args[0]);
    }
    if (arg_count >= 2 && args[1] && args[1][0]) {
        delay_ms = atoi(args[1]);
    }

    if (max_attempts < 1) max_attempts = 1;
    if (delay_ms < 0) delay_ms = 0;

    mcp_provider_request_refresh(max_attempts, delay_ms);
    snprintf(output, output_len,
             "MCP discovery refresh scheduled (max_attempts=%d delay_ms=%d). Check logs for details.",
             max_attempts, delay_ms);
    return 0;
}

static const command_t cmd_mcp_refresh = {
    .name = "mcp_refresh",
    .description = "Trigger MCP discovery refresh and rebuild tool registry JSON",
    .usage = "/mcp_refresh [max_attempts] [delay_ms]",
    .execute = cmd_mcp_refresh_execute,
};

void cmd_mcp_refresh_init(void)
{
    int ret = command_register(&cmd_mcp_refresh);
    if (ret != 0) {
        MIMI_LOGE(TAG, "Failed to register /mcp_refresh command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Registered /mcp_refresh command");
    }
}

