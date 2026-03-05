#include "cli/cli_core.h"

#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "telegram/telegram_bot.h"
#include "tools/tool_web_search.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "config/config.h"
#include "platform/fs/fs.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "cli_core";

typedef mimi_err_t (*cli_cmd_fn_t)(int argc, char **argv, const cli_io_t *io);

typedef struct cli_cmd_node {
    const char *name;
    const char *help;
    cli_cmd_fn_t fn;
    struct cli_cmd_node *next;
} cli_cmd_node_t;

static cli_cmd_node_t *s_cmd_list = NULL;

static void cli_write(const cli_io_t *io, const char *fmt, ...)
{
    if (!io || !io->write) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    io->write(io->ctx, buf);
}

/* Basic shell-like splitting with support for quotes: "..." and '...' */
static int split_argv(const char *line, char ***out_argv)
{
    *out_argv = NULL;
    if (!line) return 0;

    size_t cap = 8;
    char **argv = (char **)calloc(cap, sizeof(char *));
    if (!argv) return -1;

    int argc = 0;
    const char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p++;
        }

        const char *start = p;
        size_t len = 0;
        while (*p) {
            if (quote) {
                if (*p == quote) break;
            } else {
                if (isspace((unsigned char)*p)) break;
            }
            p++;
            len++;
        }

        char *tok = (char *)calloc(1, len + 1);
        if (!tok) {
            for (int i = 0; i < argc; i++) free(argv[i]);
            free(argv);
            return -1;
        }
        memcpy(tok, start, len);
        tok[len] = '\0';

        if (argc >= (int)cap) {
            cap *= 2;
            char **tmp = (char **)realloc(argv, cap * sizeof(char *));
            if (!tmp) {
                free(tok);
                for (int i = 0; i < argc; i++) free(argv[i]);
                free(argv);
                return -1;
            }
            argv = tmp;
        }
        argv[argc++] = tok;

        if (quote && *p == quote) p++;
    }

    *out_argv = argv;
    return argc;
}

static void free_argv(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
}

static mimi_err_t session_list_impl(int argc, char **argv, const cli_io_t *io);
static mimi_err_t session_clear_impl(int argc, char **argv, const cli_io_t *io);

static mimi_err_t cmd_help(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_set(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_memory_read(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_session(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_ask(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_exit(int argc, char **argv, const cli_io_t *io);

static mimi_err_t cmd_help(int argc, char **argv, const cli_io_t *io)
{
    (void)argc; (void)argv;
    cli_write(io, "Available commands:\n");
    cli_cmd_node_t *node = s_cmd_list;
    while (node) {
        cli_write(io, "  %s\n", node->help);
        node = node->next;
    }
    return MIMI_OK;
}

/* Unified configuration command:
 *   set api_key <KEY>
 *   set model <MODEL>
 *   set model_provider <anthropic|openai|openrouter>
 *   set tg_token <TOKEN>
 *   set search_key <BRAVE_KEY>
 */
static mimi_err_t cmd_set(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 3) {
        cli_write(io, "Usage: set <api_key|model|model_provider|tg_token|search_key> <value>\n");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *sub = argv[1];
    const char *value = argv[2];
    mimi_err_t e = MIMI_ERR_INVALID_ARG;

    if (strcmp(sub, "api_key") == 0) {
        e = llm_set_api_key(value);
    } else if (strcmp(sub, "model") == 0) {
        e = llm_set_model(value);
    } else if (strcmp(sub, "model_provider") == 0) {
        e = llm_set_provider(value);
    } else if (strcmp(sub, "tg_token") == 0) {
        e = telegram_set_token(value);
    } else if (strcmp(sub, "search_key") == 0) {
        e = tool_web_search_set_key(value);
    } else {
        cli_write(io, "Unknown set subcommand. Use: set <api_key|model|model_provider|tg_token|search_key> <value>\n");
        return MIMI_ERR_INVALID_ARG;
    }

    cli_write(io, "%s\n", (e == MIMI_OK) ? "OK" : mimi_err_to_name(e));
    return e;
}

static mimi_err_t cmd_memory_read(int argc, char **argv, const cli_io_t *io)
{
    (void)argc; (void)argv;
    char *buf = (char *)calloc(1, 4096);
    if (!buf) return MIMI_ERR_NO_MEM;
    mimi_err_t e = memory_read_long_term(buf, 4096);
    if (e == MIMI_OK && buf[0]) {
        cli_write(io, "=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        cli_write(io, "MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return MIMI_OK;
}

static mimi_err_t session_list_impl(int argc, char **argv, const cli_io_t *io)
{
    const char *channel_filter = NULL;
    bool show_all = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0) {
            show_all = true;
        } else if (strncmp(argv[i], "--channel=", 10) == 0) {
            channel_filter = argv[i] + 10;
        } else if (!channel_filter) {
            /* Bare argument treated as channel name */
            channel_filter = argv[i];
        }
    }

    const mimi_config_t *cfg = mimi_config_get();
    const char *base_dir = cfg->session_dir[0] ? cfg->session_dir : "sessions";
    mimi_dir_t *dir = NULL;
    mimi_err_t err = mimi_fs_opendir(base_dir, &dir);
    if (err != MIMI_OK) {
        cli_write(io, "Cannot open sessions directory: %s\n", base_dir);
        return MIMI_OK;
    }

    int count = 0;
    cli_write(io, "Sessions");
    if (channel_filter && !show_all) {
        cli_write(io, " (channel=%s)", channel_filter);
    }
    cli_write(io, ":\n");
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

        cli_write(io, "  - %s\n", name);
        count++;
    }
    mimi_fs_closedir(dir);

    if (count == 0) {
        cli_write(io, "  (No sessions found)\n");
    } else {
        cli_write(io, "Total: %d session(s)\n", count);
    }
    
    return MIMI_OK;
}

static mimi_err_t session_clear_impl(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 2) return MIMI_ERR_INVALID_ARG;
    /* Back-compat: if user passes "<channel>_<chat_id>" as a single token,
     * split it; otherwise assume channel="telegram". */
    const char *channel = "telegram";
    const char *chat_id = argv[1];
    char chan_buf[32];
    char id_buf[256];
    const char *sep = strchr(argv[1], '_');
    if (sep) {
        size_t clen = (size_t)(sep - argv[1]);
        if (clen > 0 && clen < sizeof(chan_buf)) {
            memcpy(chan_buf, argv[1], clen);
            chan_buf[clen] = '\0';
            strncpy(id_buf, sep + 1, sizeof(id_buf) - 1);
            id_buf[sizeof(id_buf) - 1] = '\0';
            channel = chan_buf;
            chat_id = id_buf;
        }
    }
    mimi_err_t e = session_clear(channel, chat_id);
    cli_write(io, "%s\n", (e == MIMI_OK) ? "OK" : mimi_err_to_name(e));
    return e;
}

/* High-level session command with subcommands:
 *   session list [--all|--channel=<name>|<channel>]
 *   session clear <channel>_<chat_id>
 *
 * Frontends (such as the stdio CLI) should call this instead of the
 * lower-level session_list/session_clear commands. */
static mimi_err_t cmd_session(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 2) {
        cli_write(io, "Usage: session <list|clear> [...]\n");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "list") == 0) {
        /* Reuse session_list_impl by shifting argv so that argv[0] is
         * "session_list" and the rest of the arguments are preserved. */
        char *sub_argv[argc];
        sub_argv[0] = (char *)"session_list";
        for (int i = 2; i < argc; i++) {
            sub_argv[i - 1] = argv[i];
        }
        int sub_argc = argc - 1;
        return session_list_impl(sub_argc, sub_argv, io);
    } else if (strcmp(sub, "clear") == 0) {
        char *sub_argv[argc];
        sub_argv[0] = (char *)"session_clear";
        for (int i = 2; i < argc; i++) {
            sub_argv[i - 1] = argv[i];
        }
        int sub_argc = argc - 1;
        return session_clear_impl(sub_argc, sub_argv, io);
    } else {
        cli_write(io, "Unknown session subcommand. Use: session list|clear\n");
        return MIMI_ERR_INVALID_ARG;
    }
}

static mimi_err_t cmd_ask(int argc, char **argv, const cli_io_t *io)
{
    (void)io;
    if (argc < 4) return MIMI_ERR_INVALID_ARG;
    const char *channel = argv[1];
    const char *chat_id = argv[2];

    /* Join rest into a single string */
    size_t len = 0;
    for (int i = 3; i < argc; i++) len += strlen(argv[i]) + 1;
    char *text = (char *)calloc(1, len + 1);
    if (!text) return MIMI_ERR_NO_MEM;
    for (int i = 3; i < argc; i++) {
        strcat(text, argv[i]);
        if (i != argc - 1) strcat(text, " ");
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    msg.content = text;  // transfer ownership
    mimi_err_t e = message_bus_push_inbound(&msg);
    if (e != MIMI_OK) {
        free(text);
        return e;
    }
    return MIMI_OK;
}

static mimi_err_t cmd_exit(int argc, char **argv, const cli_io_t *io)
{
    (void)argc;
    (void)argv;
    cli_write(io, "Goodbye!\n");
    return MIMI_ERR_EXIT;
}

static mimi_err_t register_builtin_commands(void)
{
    static const struct {
        const char *name;
        const char *help;
        cli_cmd_fn_t fn;
    } builtin_cmds[] = {
        {"help", "help: list commands", cmd_help},
        {"exit", "exit: quit the application", cmd_exit},
        {"set", "set <api_key|model|model_provider|tg_token|search_key> <value>", cmd_set},
        {"memory_read", "memory_read: print MEMORY.md", cmd_memory_read},
        {"session", "session <list|clear> [...]", cmd_session},
        {"ask", "ask <channel> <chat_id> <text...>: inject a message into agent", cmd_ask},
    };

    for (size_t i = 0; i < sizeof(builtin_cmds) / sizeof(builtin_cmds[0]); i++) {
        mimi_err_t e = cli_register_cmd(builtin_cmds[i].name, builtin_cmds[i].help, builtin_cmds[i].fn);
        if (e != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to register builtin command: %s", builtin_cmds[i].name);
            return e;
        }
    }
    return MIMI_OK;
}

mimi_err_t cli_core_init(void)
{
    mimi_err_t e = register_builtin_commands();
    if (e != MIMI_OK) return e;
    
    int count = 0;
    cli_cmd_node_t *node = s_cmd_list;
    while (node) {
        count++;
        node = node->next;
    }
    
    MIMI_LOGI(TAG, "CLI core ready (%d commands)", count);
    return MIMI_OK;
}

mimi_err_t cli_core_execute_line(const char *line, const cli_io_t *io)
{
    if (!line) return MIMI_ERR_INVALID_ARG;

    /* trim leading whitespace */
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') return MIMI_OK;

    char **argv = NULL;
    int argc = split_argv(line, &argv);
    if (argc < 0) return MIMI_ERR_NO_MEM;
    if (argc == 0) {
        free(argv);
        return MIMI_OK;
    }

    mimi_err_t rc = MIMI_ERR_NOT_FOUND;
    cli_cmd_node_t *node = s_cmd_list;
    while (node) {
        if (strcmp(argv[0], node->name) == 0) {
            rc = node->fn(argc, argv, io);
            break;
        }
        node = node->next;
    }

    if (rc == MIMI_ERR_NOT_FOUND) {
        cli_write(io, "Unknown command: %s\n", argv[0]);
    } else if (rc == MIMI_ERR_INVALID_ARG) {
        cli_write(io, "Invalid args. Try: help\n");
    }

    free_argv(argc, argv);
    return rc;
}

mimi_err_t cli_register_cmd(const char *name, const char *help, cli_cmd_fn_t fn)
{
    if (!name || !fn) return MIMI_ERR_INVALID_ARG;
    
    /* Check if command already exists */
    cli_cmd_node_t *node = s_cmd_list;
    while (node) {
        if (strcmp(node->name, name) == 0) {
            return MIMI_ERR_INVALID_STATE;
        }
        node = node->next;
    }
    
    /* Create new command node */
    cli_cmd_node_t *new_node = (cli_cmd_node_t *)calloc(1, sizeof(cli_cmd_node_t));
    if (!new_node) return MIMI_ERR_NO_MEM;
    
    new_node->name = name;
    new_node->help = help;
    new_node->fn = fn;
    new_node->next = s_cmd_list;
    
    /* Add to head of list */
    s_cmd_list = new_node;
    
    return MIMI_OK;
}

int cli_get_completions(const char *prefix, char **out_matches, int max_matches)
{
    if (!prefix || !out_matches || max_matches <= 0) return 0;
    
    int count = 0;
    size_t prefix_len = strlen(prefix);
    cli_cmd_node_t *node = s_cmd_list;
    
    while (node && count < max_matches) {
        if (strncmp(node->name, prefix, prefix_len) == 0) {
            out_matches[count] = strdup(node->name);
            if (out_matches[count]) {
                count++;
            }
        }
        node = node->next;
    }
    
    return count;
}

