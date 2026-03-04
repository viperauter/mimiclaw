#include "cli/cli_core.h"

#include "platform/log.h"
#include "platform/os.h"
#include "platform/time.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "stdio_cli";

static void stdio_write(void *ctx, const char *text)
{
    (void)ctx;
    fputs(text, stdout);
    fflush(stdout);
}

static void stdio_cli_task(void *arg)
{
    (void)arg;
    cli_io_t io = {.write = stdio_write, .ctx = NULL};

    MIMI_LOGI(TAG, "POSIX CLI ready. Type 'help'.");
    for (;;) {
        fputs("> ", stdout);
        fflush(stdout);

        char line[2048];
        if (!fgets(line, sizeof(line), stdin)) {
            mimi_sleep_ms(200);
            continue;
        }

        /* Strip newline */
        size_t n = strlen(line);
        if (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[n - 1] = '\0';

        cli_core_execute_line(line, &io);
    }
}

mimi_err_t stdio_cli_start(void)
{
    mimi_err_t e = cli_core_init();
    if (e != MIMI_OK) return e;
    return mimi_task_create_detached("stdio_cli", stdio_cli_task, NULL);
}

