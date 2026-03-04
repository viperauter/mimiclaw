#include "cli/cli_core.h"

#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "telegram/telegram_bot.h"
#include "tools/tool_web_search.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "platform/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "cli_core";

typedef mimi_err_t (*cli_cmd_fn_t)(int argc, char **argv, const cli_io_t *io);

typedef struct {
    const char *name;
    const char *help;
    cli_cmd_fn_t fn;
} cli_cmd_t;

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

static mimi_err_t cmd_help(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_set_api_key(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_set_model(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_set_provider(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_set_tg_token(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_set_search_key(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_memory_read(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_session_list(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_session_clear(int argc, char **argv, const cli_io_t *io);
static mimi_err_t cmd_ask(int argc, char **argv, const cli_io_t *io);

static const cli_cmd_t s_cmds[] = {
    {"help", "help: list commands", cmd_help},
    {"set_api_key", "set_api_key <KEY>", cmd_set_api_key},
    {"set_model", "set_model <MODEL>", cmd_set_model},
    {"set_model_provider", "set_model_provider <anthropic|openai|openrouter>", cmd_set_provider},
    {"set_tg_token", "set_tg_token <TOKEN>", cmd_set_tg_token},
    {"set_search_key", "set_search_key <BRAVE_KEY>", cmd_set_search_key},
    {"memory_read", "memory_read: print MEMORY.md", cmd_memory_read},
    {"session_list", "session_list: list sessions", cmd_session_list},
    {"session_clear", "session_clear <chat_id>", cmd_session_clear},
    {"ask", "ask <channel> <chat_id> <text...>: inject a message into agent", cmd_ask},
};

static mimi_err_t cmd_help(int argc, char **argv, const cli_io_t *io)
{
    (void)argc; (void)argv;
    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        cli_write(io, "%s\n", s_cmds[i].help);
    }
    return MIMI_OK;
}

static mimi_err_t cmd_set_api_key(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 2) return MIMI_ERR_INVALID_ARG;
    mimi_err_t e = llm_set_api_key(argv[1]);
    cli_write(io, "%s\n", (e == MIMI_OK) ? "OK" : mimi_err_to_name(e));
    return e;
}

static mimi_err_t cmd_set_model(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 2) return MIMI_ERR_INVALID_ARG;
    mimi_err_t e = llm_set_model(argv[1]);
    cli_write(io, "%s\n", (e == MIMI_OK) ? "OK" : mimi_err_to_name(e));
    return e;
}

static mimi_err_t cmd_set_provider(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 2) return MIMI_ERR_INVALID_ARG;
    mimi_err_t e = llm_set_provider(argv[1]);
    cli_write(io, "%s\n", (e == MIMI_OK) ? "OK" : mimi_err_to_name(e));
    return e;
}

static mimi_err_t cmd_set_tg_token(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 2) return MIMI_ERR_INVALID_ARG;
    mimi_err_t e = telegram_set_token(argv[1]);
    cli_write(io, "%s\n", (e == MIMI_OK) ? "OK" : mimi_err_to_name(e));
    return e;
}

static mimi_err_t cmd_set_search_key(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 2) return MIMI_ERR_INVALID_ARG;
    mimi_err_t e = tool_web_search_set_key(argv[1]);
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

static mimi_err_t cmd_session_list(int argc, char **argv, const cli_io_t *io)
{
    (void)argc; (void)argv;
    (void)io;
    session_list();
    return MIMI_OK;
}

static mimi_err_t cmd_session_clear(int argc, char **argv, const cli_io_t *io)
{
    if (argc < 2) return MIMI_ERR_INVALID_ARG;
    mimi_err_t e = session_clear(argv[1]);
    cli_write(io, "%s\n", (e == MIMI_OK) ? "OK" : mimi_err_to_name(e));
    return e;
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

mimi_err_t cli_core_init(void)
{
    MIMI_LOGI(TAG, "CLI core ready (%u commands)", (unsigned)(sizeof(s_cmds) / sizeof(s_cmds[0])));
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
    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        if (strcmp(argv[0], s_cmds[i].name) == 0) {
            rc = s_cmds[i].fn(argc, argv, io);
            break;
        }
    }

    if (rc == MIMI_ERR_NOT_FOUND) {
        cli_write(io, "Unknown command: %s\n", argv[0]);
    } else if (rc == MIMI_ERR_INVALID_ARG) {
        cli_write(io, "Invalid args. Try: help\n");
    }

    free_argv(argc, argv);
    return rc;
}

