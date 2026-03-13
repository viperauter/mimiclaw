/**
 * Session Command Implementation
 *
 * Provides session management commands:
 *   /session list [--all|--channel=<name>|<channel>]
 *   /session clear <channel>_<chat_id>
 */

#include "commands/command.h"
#include "fs/fs.h"
#include "config/config.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cmd_session";

/**
 * List sessions implementation
 */
static int session_list_execute(const char **args, int arg_count,
                                const command_context_t *ctx,
                                char *output, size_t output_len)
{
    (void)ctx;

    const char *channel_filter = NULL;
    bool show_all = false;

    /* Parse arguments */
    for (int i = 0; i < arg_count; i++) {
        if (strcmp(args[i], "--all") == 0) {
            show_all = true;
        } else if (strncmp(args[i], "--channel=", 10) == 0) {
            channel_filter = args[i] + 10;
        } else if (!channel_filter) {
            /* Bare argument treated as channel name */
            channel_filter = args[i];
        }
    }

    const mimi_config_t *cfg = mimi_config_get();
    const char *base_dir = cfg->session_dir[0] ? cfg->session_dir : "sessions";

    mimi_dir_t *dir = NULL;
    mimi_err_t err = mimi_fs_opendir(base_dir, &dir);
    if (err != MIMI_OK) {
        snprintf(output, output_len, "Cannot open sessions directory: %s", base_dir);
        return -1;
    }

    size_t pos = 0;
    pos += snprintf(output + pos, output_len - pos, "Sessions");
    if (channel_filter && !show_all) {
        pos += snprintf(output + pos, output_len - pos, " (channel=%s)", channel_filter);
    }
    pos += snprintf(output + pos, output_len - pos, ":\n");

    int count = 0;
    for (;;) {
        bool has = false;
        char name[256];
        err = mimi_fs_readdir(dir, name, sizeof(name), &has);
        if (err != MIMI_OK) break;
        if (!has) break;

        /* List JSONL session files: "<channel>_<chat_id>.jsonl" */
        const size_t n = strlen(name);
        if (n < 6 || strcmp(name + n - 6, ".jsonl") != 0) {
            continue;
        }

        if (!show_all && channel_filter) {
            size_t clen = strlen(channel_filter);
            if (clen == 0 || n <= clen + 1) continue;
            if (strncmp(name, channel_filter, clen) != 0 || name[clen] != '_') {
                continue;
            }
        }

        pos += snprintf(output + pos, output_len - pos, "  - %s\n", name);
        count++;
    }
    mimi_fs_closedir(dir);

    if (count == 0) {
        pos += snprintf(output + pos, output_len - pos, "  (No sessions found)\n");
    } else {
        pos += snprintf(output + pos, output_len - pos, "Total: %d session(s)\n", count);
    }

    return 0;
}

/**
 * Clear session implementation
 */
static int session_clear_execute(const char **args, int arg_count,
                                 const command_context_t *ctx,
                                 char *output, size_t output_len)
{
    (void)ctx;

    if (arg_count < 1) {
        snprintf(output, output_len, "Usage: /session clear <channel>_<chat_id>");
        return -1;
    }

    /* Back-compat: if user passes "<channel>_<chat_id>" as a single token,
     * split it; otherwise assume channel="telegram" */
    const char *channel = "telegram";
    const char *chat_id = args[0];
    char chan_buf[32];
    char id_buf[256];

    const char *sep = strchr(args[0], '_');
    if (sep) {
        size_t clen = (size_t)(sep - args[0]);
        if (clen > 0 && clen < sizeof(chan_buf)) {
            memcpy(chan_buf, args[0], clen);
            chan_buf[clen] = '\0';
            strncpy(id_buf, sep + 1, sizeof(id_buf) - 1);
            id_buf[sizeof(id_buf) - 1] = '\0';
            channel = chan_buf;
            chat_id = id_buf;
        }
    }

    /* Call session_clear from session_mgr */
    extern mimi_err_t session_clear(const char *channel, const char *chat_id);
    mimi_err_t err = session_clear(channel, chat_id);

    if (err == MIMI_OK) {
        snprintf(output, output_len, "OK");
    } else {
        snprintf(output, output_len, "Error: %s", mimi_err_to_name(err));
    }

    return (err == MIMI_OK) ? 0 : -1;
}

/**
 * Main session command dispatcher
 */
static int cmd_session_execute(const char **args, int arg_count,
                               const command_context_t *ctx,
                               char *output, size_t output_len)
{
    if (arg_count < 1) {
        snprintf(output, output_len,
                 "Usage: /session <list|clear> [...]\n"
                 "  list [--all|--channel=<name>|<channel>]\n"
                 "  clear <channel>_<chat_id>");
        return -1;
    }

    const char *sub = args[0];

    if (strcmp(sub, "list") == 0) {
        return session_list_execute(args + 1, arg_count - 1, ctx, output, output_len);
    } else if (strcmp(sub, "clear") == 0) {
        return session_clear_execute(args + 1, arg_count - 1, ctx, output, output_len);
    }

    snprintf(output, output_len, "Unknown subcommand: %s\nUse: list, clear", sub);
    return -1;
}

/* Command definition */
static const command_t cmd_session = {
    .name = "session",
    .description = "Session management (list, clear)",
    .usage = "/session <list|clear> [...]",
    .execute = cmd_session_execute,
};

/**
 * Initialize session command
 */
void cmd_session_init(void)
{
    int ret = command_register(&cmd_session);
    if (ret != 0) {
        MIMI_LOGW(TAG, "Failed to register session command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Session command registered");
    }
}
