#include "cli/cli_core.h"

#include "bus/message_bus.h"
#include "log.h"
#include "os/os.h"
#include "mimi_time.h"
#include "platform/runtime.h"
#include "memory/session_mgr.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "linenoise/linenoise.h"

static const char *TAG = "cli";

/* Exposed for async writers (logs, agent output) to temporarily hide/show the
 * prompt while the user is editing a line. */
struct linenoiseState *g_linenoise_state = NULL;

/* Current interactive session for CLI Studio. All plain-text (non-command)
 * input is sent as chat to this (channel, chat_id). */
static char s_current_channel[16] = "cli";
static char s_current_chat_id[32] = "default";

static void stdio_write(void *ctx, const char *text)
{
    (void)ctx;
    fputs(text, stdout);
    fflush(stdout);
}

/* Completion callback for linenoise */
static void cli_completion_callback(const char *text, linenoiseCompletions *lc)
{
    const char *prefix = text;
    bool has_slash = false;
    if (*prefix == '/') {
        has_slash = true;
        prefix++;
        while (*prefix == ' ') prefix++;
    }
    char *matches[32];
    int match_count = cli_get_completions(prefix, matches, 32);
    
    for (int i = 0; i < match_count; i++) {
        if (has_slash) {
            char buf[64];
            snprintf(buf, sizeof(buf), "/%s", matches[i]);
            linenoiseAddCompletion(lc, buf);
        } else {
            linenoiseAddCompletion(lc, matches[i]);
        }
    }
}

static void stdio_cli_task(void *arg)
{
    (void)arg;
    cli_io_t io = {.write = stdio_write, .ctx = NULL};

    MIMI_LOGI(TAG, "POSIX CLI ready. Type 'help' for available commands.");
    
    /* Setup linenoise */
    linenoiseSetCompletionCallback(cli_completion_callback);
    linenoiseHistorySetMaxLen(100);
    
    for (;;) {
        /* Create prompt with timestamp */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char timestamp[20];
        strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
        
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "[%s] mimiclaw(%s:%s)> ",
                 timestamp, s_current_channel, s_current_chat_id);

        /* Read line using the multiplexed API so other threads can safely
         * hide/show the prompt while printing async output. */
        char input_buf[4096];
        struct linenoiseState ls;
        if (linenoiseEditStart(&ls, -1, -1, input_buf, sizeof(input_buf), prompt) == -1) {
            mimi_sleep_ms(200);
            continue;
        }
        g_linenoise_state = &ls;

        char *line = NULL;
        for (;;) {
            char *res = linenoiseEditFeed(&ls);
            if (res == linenoiseEditMore) continue;
            line = res;
            break;
        }
        g_linenoise_state = NULL;
        linenoiseEditStop(&ls);

        if (!line) {
            mimi_sleep_ms(200);
            continue;
        }
        
        /* Skip empty lines */
        if (strlen(line) == 0) {
            linenoiseFree(line);
            continue;
        }
        
        /* Add to history */
        linenoiseHistoryAdd(line);

        mimi_err_t result = MIMI_OK;

        /* Determine if this is a CLI command or a chat message. */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        if (*p == '/') {
            /* Strip leading '/' and optional whitespace. */
            p++;
            while (*p && isspace((unsigned char)*p)) p++;

            /* High-level session helpers: /session list|new|use|clear ... */
            if (strncmp(p, "session", 7) == 0 && (p[7] == '\0' || isspace((unsigned char)p[7]))) {
                char *sub = p + 7;
                while (*sub && isspace((unsigned char)*sub)) sub++;

                if (*sub == '\0') {
                    stdio_write(io.ctx, "Usage: /session <list|new|use|clear> [...]\n");
                } else if (strncmp(sub, "list", 4) == 0 && (sub[4] == '\0' || isspace((unsigned char)sub[4]))) {
                    char *args = sub + 4;
                    while (*args && isspace((unsigned char)*args)) args++;

                    /* /session list --all  -> list all sessions
                     * /session list        -> list only current channel sessions
                     */
                    if (strncmp(args, "--all", 5) == 0 && (args[5] == '\0' || isspace((unsigned char)args[5]))) {
                        result = cli_core_execute_line("session list --all", &io);
                    } else {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "session list %s", s_current_channel);
                        result = cli_core_execute_line(buf, &io);
                    }
                } else if (strncmp(sub, "new", 3) == 0 && (sub[3] == '\0' || isspace((unsigned char)sub[3]))) {
                    char *id = sub + 3;
                    while (*id && isspace((unsigned char)*id)) id++;
                    if (*id) {
                        /* Explicitly create a new session by appending a meta
                         * "system" entry, then switch to it. */
                        mimi_err_t e = session_append(s_current_channel, id, "system", "session created");
                        if (e == MIMI_OK) {
                            strncpy(s_current_chat_id, id, sizeof(s_current_chat_id) - 1);
                            s_current_chat_id[sizeof(s_current_chat_id) - 1] = '\0';
                            stdio_write(io.ctx, "Created and switched session to ");
                            stdio_write(io.ctx, s_current_chat_id);
                            stdio_write(io.ctx, "\n");
                        } else {
                            stdio_write(io.ctx, "Failed to create session\n");
                        }
                    } else {
                        stdio_write(io.ctx, "Usage: /session new <chat_id>\n");
                    }
                } else if (strncmp(sub, "use", 3) == 0 && (sub[3] == '\0' || isspace((unsigned char)sub[3]))) {
                    char *id = sub + 3;
                    while (*id && isspace((unsigned char)*id)) id++;
                    if (*id) {
                        strncpy(s_current_chat_id, id, sizeof(s_current_chat_id) - 1);
                        s_current_chat_id[sizeof(s_current_chat_id) - 1] = '\0';
                        stdio_write(io.ctx, "Switched session to ");
                        stdio_write(io.ctx, s_current_chat_id);
                        stdio_write(io.ctx, "\n");
                    } else {
                        stdio_write(io.ctx, "Usage: /session use <chat_id>\n");
                    }
                } else if (strncmp(sub, "clear", 5) == 0 && (sub[5] == '\0' || isspace((unsigned char)sub[5]))) {
                    char *id = sub + 5;
                    while (*id && isspace((unsigned char)*id)) id++;
                    /* Strip trailing .jsonl if user pasted filename */
                    size_t idlen = strlen(id);
                    if (idlen > 6 && strcmp(id + idlen - 6, ".jsonl") == 0) id[idlen - 6] = '\0';

                    char buf[256];
                    if (*id) {
                        /* If id contains '_' it's channel_chatid (e.g. tg_test); pass as-is.
                         * Else it's a chat_id in current channel; prepend channel. */
                        if (strchr(id, '_') != NULL) {
                            snprintf(buf, sizeof(buf), "session clear %s", id);
                        } else {
                            snprintf(buf, sizeof(buf), "session clear %s_%s", s_current_channel, id);
                        }
                    } else {
                        snprintf(buf, sizeof(buf), "session clear %s_%s", s_current_channel, s_current_chat_id);
                    }
                    result = cli_core_execute_line(buf, &io);
                } else {
                    stdio_write(io.ctx, "Unknown /session subcommand. Use: list|new|use|clear\n");
                }
            } else {
                /* Other commands: pass through to cli_core (e.g., help, set_model, exit). */
                result = cli_core_execute_line(p, &io);
            }
        } else {
            /* Plain text: treat as chat message to current (channel, chat_id). */
            mimi_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            strncpy(msg.channel, s_current_channel, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, s_current_chat_id, sizeof(msg.chat_id) - 1);

            size_t len = strlen(p);
            char *text = (char *)calloc(1, len + 1);
            if (!text) {
                stdio_write(io.ctx, "Error: out of memory\n");
            } else {
                memcpy(text, p, len);
                msg.content = text;
                mimi_err_t e = message_bus_push_inbound(&msg);
                if (e != MIMI_OK) {
                    stdio_write(io.ctx, "Failed to enqueue message\n");
                    free(text);
                }
            }
        }
        
        linenoiseFree(line);
        
        /* Check if exit command was issued */
        if (result == MIMI_ERR_EXIT) {
            MIMI_LOGI(TAG, "Exit requested, signaling runtime to shutdown...");
            mimi_runtime_request_exit();
            break;
        }
    }
}

mimi_err_t stdio_cli_start(void)
{
    mimi_err_t e = cli_core_init();
    if (e != MIMI_OK) return e;
    return mimi_task_create_detached("stdio_cli", stdio_cli_task, NULL);
}

