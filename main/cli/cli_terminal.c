/**
 * Generic CLI Terminal Framework
 * Plugin architecture for different transport layers
 */

#include "cli/cli_terminal.h"
#include "cli/editor.h"
#include "commands/command.h"
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

#ifdef _WIN32
#include <windows.h>
#endif

#define MAX_TERMINALS 8
#define TAG "cli_terminal"

/* Forward declarations for command completion */
static int cli_get_completions(const char *prefix, char **out_matches, int max_matches);

/* Terminal instance structure */
struct app_terminal {
    char name[64];
    char channel[32];
    char chat_id[32];
    cli_transport_t transport;
    cli_terminal_t *editor_term;  /* Underlying editor terminal */
};

/* Global state */
static struct {
    app_terminal_t *terminals[MAX_TERMINALS];
    int count;
    bool initialized;
} g_state = {0};

/* Current active terminal for callback context */
static __thread app_terminal_t *s_current_term = NULL;

/* Forward declarations */
static void cli_execute_wrapper(const char *line, void *user_data);
static int cli_complete_wrapper(const char *prefix, char **matches, int max_matches, void *user_data);
static const char* cli_get_prompt_wrapper(void *user_data);
static void cli_output_adapter(void *user_data, const char *str);

/* Output adapter for editor library */
static void cli_output_adapter(void *user_data, const char *str)
{
    app_terminal_t *term = (app_terminal_t *)user_data;
    if (term && str) {
        term->transport.write(term->transport.ctx, str, strlen(str));
    }
}

/* Get prompt callback */
static const char* cli_get_prompt_wrapper(void *user_data)
{
    app_terminal_t *term = (app_terminal_t *)user_data;
    static char prompt[256];
    
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);
    
    snprintf(prompt, sizeof(prompt), "[%s] mimiclaw(%s:%s)> ",
             timestamp, term->channel, term->chat_id);
    
    return prompt;
}

/* Execute callback */
static void cli_execute_wrapper(const char *line, void *user_data)
{
    app_terminal_t *term = (app_terminal_t *)user_data;
    s_current_term = term;
    
    if (!line || !*line) {
        cli_terminal_print_prompt(term->editor_term);
        return;
    }
    
    /* Check if this is a command (starts with "/") or chat message */
    if (line[0] == '/') {
        /* It's a command - strip the leading "/" */
        const char *cmd = line + 1;
        while (*cmd && isspace((unsigned char)*cmd)) cmd++;
        
        if (!*cmd) {
            cli_output_ln("Usage: /<command> [args...]");
            cli_terminal_print_prompt(term->editor_term);
            return;
        }
        
        /* Handle session commands inline for better UX */
        if (strncmp(cmd, "session", 7) == 0) {
            const char *sub = cmd + 7;
            while (*sub && isspace((unsigned char)*sub)) sub++;
            
            if (!*sub) {
                cli_output_ln("Usage: /session <list|new|use|clear> [...]");
            } else if (strncmp(sub, "list", 4) == 0) {
                const char *args = sub + 4;
                while (*args && isspace((unsigned char)*args)) args++;
                
                /* Use new command system */
                command_context_t ctx = {
                    .channel = term->channel,
                    .session_id = term->chat_id,
                    .user_id = "local_user",
                    .user_data = term,
                };
                char output[COMMAND_OUTPUT_MAX_LEN];

                if (strncmp(args, "--all", 5) == 0) {
                    command_execute("/session list --all", &ctx, output, sizeof(output));
                } else {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "/session list %s", term->channel);
                    command_execute(buf, &ctx, output, sizeof(output));
                }
                if (output[0]) {
                    cli_output_ln(output);
                }
            } else if (strncmp(sub, "new", 3) == 0) {
                const char *id = sub + 3;
                while (*id && isspace((unsigned char)*id)) id++;
                if (*id) {
                    mimi_err_t e = session_append(term->channel, id, "system", "session created");
                    if (e == MIMI_OK) {
                        strncpy(term->chat_id, id, sizeof(term->chat_id) - 1);
                        term->chat_id[sizeof(term->chat_id) - 1] = '\0';
                        cli_output("Created and switched session to ");
                        cli_output(term->chat_id);
                        cli_output_ln("");
                    } else {
                        cli_output_ln("Failed to create session");
                    }
                } else {
                    cli_output_ln("Usage: /session new <chat_id>");
                }
            } else if (strncmp(sub, "use", 3) == 0) {
                const char *id = sub + 3;
                while (*id && isspace((unsigned char)*id)) id++;
                if (*id) {
                    strncpy(term->chat_id, id, sizeof(term->chat_id) - 1);
                    term->chat_id[sizeof(term->chat_id) - 1] = '\0';
                    cli_output("Switched session to ");
                    cli_output(term->chat_id);
                    cli_output_ln("");
                } else {
                    cli_output_ln("Usage: /session use <chat_id>");
                }
            } else if (strncmp(sub, "clear", 5) == 0) {
                const char *id = sub + 5;
                while (*id && isspace((unsigned char)*id)) id++;
                
                char id_buf[64];
                strncpy(id_buf, id, sizeof(id_buf) - 1);
                id_buf[sizeof(id_buf) - 1] = '\0';
                
                size_t idlen = strlen(id_buf);
                if (idlen > 6 && strcmp(id_buf + idlen - 6, ".jsonl") == 0) {
                    id_buf[idlen - 6] = '\0';
                }
                
                char buf[256];
                if (*id_buf) {
                    if (strchr(id_buf, '_') != NULL) {
                        snprintf(buf, sizeof(buf), "/session clear %s", id_buf);
                    } else {
                        snprintf(buf, sizeof(buf), "/session clear %s_%s", term->channel, id_buf);
                    }
                } else {
                    snprintf(buf, sizeof(buf), "/session clear %s_%s", term->channel, term->chat_id);
                }
                /* Use new command system */
                command_context_t clear_ctx = {
                    .channel = term->channel,
                    .session_id = term->chat_id,
                    .user_id = "local_user",
                    .user_data = term,
                };
                char clear_output[COMMAND_OUTPUT_MAX_LEN];
                command_execute(buf, &clear_ctx, clear_output, sizeof(clear_output));
                if (clear_output[0]) {
                    cli_output_ln(clear_output);
                }
            } else {
                cli_output_ln("Unknown /session subcommand. Use: list|new|use|clear");
            }
        } else {
            /* Other commands: use new command system */
            command_context_t ctx = {
                .channel = term->channel,
                .session_id = term->chat_id,
                .user_id = "local_user",
                .user_data = term,
            };

            char output[COMMAND_OUTPUT_MAX_LEN];
            int ret = command_execute(line, &ctx, output, sizeof(output));

            if (ret == -100) {
                /* Exit command */
                MIMI_LOGI(TAG, "Exit requested, signaling runtime to shutdown...");
                mimi_runtime_request_exit();
                return;
            }

            if (output[0]) {
                cli_output_ln(output);
            }
        }
    } else {
        /* Plain text: treat as chat message */
        mimi_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, term->channel, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, term->chat_id, sizeof(msg.chat_id) - 1);
        
        size_t len = strlen(line);
        char *text = (char *)calloc(1, len + 1);
        if (!text) {
            cli_output_ln("Error: out of memory");
        } else {
            memcpy(text, line, len);
            msg.content = text;
            mimi_err_t e = message_bus_push_inbound(&msg);
            if (e != MIMI_OK) {
                cli_output_ln("Failed to enqueue message");
                free(text);
            }
        }
    }
    
    cli_terminal_print_prompt(term->editor_term);
}

/* Tab completion callback */
/* Get command completions from the new command system */
static int cli_get_completions(const char *prefix, char **out_matches, int max_matches)
{
    if (!prefix || !out_matches || max_matches <= 0) {
        return 0;
    }

    /* Skip leading '/' if present */
    if (prefix[0] == '/') {
        prefix++;
    }

    int count = 0;
    int cmd_count = command_get_count();

    for (int i = 0; i < cmd_count && count < max_matches; i++) {
        const command_t *cmd = command_get_by_index(i);
        if (cmd && cmd->name) {
            /* Check if command name starts with prefix */
            if (strncmp(cmd->name, prefix, strlen(prefix)) == 0) {
                out_matches[count] = strdup(cmd->name);
                if (out_matches[count]) {
                    count++;
                }
            }
        }
    }

    return count;
}

static int cli_complete_wrapper(const char *prefix, char **matches, int max_matches, void *user_data)
{
    (void)user_data;

    if (!prefix || !matches || max_matches <= 0) return 0;

    /* Get completions from command system */
    return cli_get_completions(prefix, matches, max_matches);
}

mimi_err_t app_terminal_init(void)
{
    if (g_state.initialized) {
        return MIMI_OK;
    }

    /* Note: Command system is now initialized in main() */
    /* Initialize editor library */
    cli_init(cli_execute_wrapper, cli_complete_wrapper, cli_get_prompt_wrapper);

    g_state.initialized = true;
    g_state.count = 0;

    MIMI_LOGI(TAG, "CLI terminal framework initialized");
    return MIMI_OK;
}

app_terminal_t* app_terminal_create(const app_terminal_config_t *config)
{
    if (!g_state.initialized) {
        MIMI_LOGE(TAG, "CLI terminal not initialized");
        return NULL;
    }
    
    if (g_state.count >= MAX_TERMINALS) {
        MIMI_LOGE(TAG, "Max terminals reached");
        return NULL;
    }
    
    app_terminal_t *term = calloc(1, sizeof(app_terminal_t));
    if (!term) {
        MIMI_LOGE(TAG, "Failed to allocate terminal");
        return NULL;
    }
    
    /* Copy configuration */
    strncpy(term->name, config->name, sizeof(term->name) - 1);
    strncpy(term->channel, config->channel ? config->channel : "cli", sizeof(term->channel) - 1);
    strncpy(term->chat_id, config->chat_id ? config->chat_id : "default", sizeof(term->chat_id) - 1);
    term->transport = config->transport;
    
    /* Create underlying editor terminal */
    term->editor_term = cli_terminal_create(CLI_TERMINAL_CUSTOM, term, NULL, cli_output_adapter);
    if (!term->editor_term) {
        MIMI_LOGE(TAG, "Failed to create editor terminal");
        free(term);
        return NULL;
    }
    
    /* Add to global list */
    g_state.terminals[g_state.count++] = term;

    MIMI_LOGI(TAG, "Created terminal '%s' (%s:%s)", term->name, term->channel, term->chat_id);

    /* Print initial prompt */
    s_current_term = term;
    cli_terminal_print_prompt(term->editor_term);

    return term;
}

void app_terminal_destroy(app_terminal_t *term)
{
    if (!term) return;
    
    /* Remove from global list */
    for (int i = 0; i < g_state.count; i++) {
        if (g_state.terminals[i] == term) {
            /* Shift remaining terminals */
            for (int j = i; j < g_state.count - 1; j++) {
                g_state.terminals[j] = g_state.terminals[j + 1];
            }
            g_state.count--;
            break;
        }
    }
    
    /* Destroy editor terminal */
    if (term->editor_term) {
        cli_terminal_destroy(term->editor_term);
    }
    
    /* Close transport */
    if (term->transport.close) {
        term->transport.close(term->transport.ctx);
    }
    
    MIMI_LOGI(TAG, "Destroyed terminal '%s'", term->name);
    free(term);
}

void app_terminal_poll_all(void)
{
    if (!g_state.initialized) return;
    
    bool should_exit = false;
    
    for (int i = 0; i < g_state.count && !should_exit; i++) {
        app_terminal_t *term = g_state.terminals[i];
        s_current_term = term;
        
        /* Check if data available */
        if (term->transport.available && !term->transport.available(term->transport.ctx)) {
            continue;
        }
        
        /* Read and feed to editor */
        char buf[16];
        int n = term->transport.read(term->transport.ctx, buf, sizeof(buf));
        if (n > 0) {
            for (int j = 0; j < n && !should_exit; j++) {
                /* Check for EOT (Ctrl+D, 0x04) - exit command */
                if (buf[j] == 0x04) {
                    MIMI_LOGI(TAG, "EOT (Ctrl+D) received, requesting exit");
                    /* Output newline to ensure clean shell prompt */
                    term->transport.write(term->transport.ctx, "\n", 1);
                    mimi_runtime_request_exit();
                    should_exit = true;
                    break;
                }
                cli_terminal_feed_char(term->editor_term, buf[j]);
            }
        } else if (n < 0) {
            /* Error - mark for removal */
            MIMI_LOGW(TAG, "Terminal '%s' read error, removing", term->name);
            app_terminal_destroy(term);
            i--; /* Adjust index since we removed current */
        } else if (n == 0) {
            /* EOF - only request exit if not already requested */
            if (!mimi_runtime_should_exit()) {
                MIMI_LOGI(TAG, "EOF received, requesting exit");
                /* Output newline to ensure clean shell prompt */
                term->transport.write(term->transport.ctx, "\n", 1);
                mimi_runtime_request_exit();
            }
            should_exit = true;
            break; /* Exit loop */
        }
    }
    
    /* Also poll the editor's internal terminals */
    cli_poll();
}

void app_terminal_output(app_terminal_t *term, const char *text)
{
    if (!term || !text) return;
    term->transport.write(term->transport.ctx, text, strlen(text));
}

void app_terminal_output_ln(app_terminal_t *term, const char *text)
{
    if (!term) return;
    if (text) {
        term->transport.write(term->transport.ctx, text, strlen(text));
    }
    term->transport.write(term->transport.ctx, "\n", 1);
}

const char* app_terminal_get_name(app_terminal_t *term)
{
    return term ? term->name : NULL;
}

int app_terminal_count(void)
{
    return g_state.count;
}
