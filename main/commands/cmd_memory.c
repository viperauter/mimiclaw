/**
 * Memory Command Implementation
 *
 * Provides memory management commands:
 *   /memory_read - Print MEMORY.md content
 */

#include "commands/command.h"
#include "memory/memory_store.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "cmd_memory";

/**
 * Execute memory_read command
 */
static int cmd_memory_read_execute(const char **args, int arg_count,
                                   const command_context_t *ctx,
                                   char *output, size_t output_len)
{
    (void)args;
    (void)arg_count;
    (void)ctx;

    char *buf = (char *)calloc(1, 4096);
    if (!buf) {
        snprintf(output, output_len, "Error: Out of memory");
        return -1;
    }

    mimi_err_t err = memory_read_long_term(buf, 4096);

    if (err == MIMI_OK && buf[0]) {
        snprintf(output, output_len,
                 "=== MEMORY.md ===\n%s\n=================", buf);
    } else {
        snprintf(output, output_len, "MEMORY.md is empty or not found.");
    }

    free(buf);
    return 0;
}

/* Command definition */
static const command_t cmd_memory_read = {
    .name = "memory_read",
    .description = "Read long-term memory (MEMORY.md)",
    .usage = "/memory_read",
    .execute = cmd_memory_read_execute,
};

/**
 * Initialize memory_read command
 */
void cmd_memory_read_init(void)
{
    int ret = command_register(&cmd_memory_read);
    if (ret != 0) {
        MIMI_LOGW(TAG, "Failed to register memory_read command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Memory_read command registered");
    }
}
