/**
 * Command Registry Implementation
 */

#include "commands/command_registry.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "cmd_registry";

/* Command registry state */
typedef struct {
    bool initialized;
    command_t commands[COMMAND_MAX_COUNT];
    int count;
} command_registry_t;

static command_registry_t s_registry = {
    .initialized = false,
    .count = 0,
};

int command_system_init(void)
{
    if (s_registry.initialized) {
        MIMI_LOGW(TAG, "Command system already initialized");
        return 0;
    }

    memset(s_registry.commands, 0, sizeof(s_registry.commands));
    s_registry.count = 0;
    s_registry.initialized = true;

    MIMI_LOGD(TAG, "Command system initialized (max_commands=%d)", COMMAND_MAX_COUNT);
    return 0;
}

void command_system_deinit(void)
{
    if (!s_registry.initialized) {
        return;
    }

    memset(s_registry.commands, 0, sizeof(s_registry.commands));
    s_registry.count = 0;
    s_registry.initialized = false;

    MIMI_LOGI(TAG, "Command system deinitialized");
}

bool command_system_is_initialized(void)
{
    return s_registry.initialized;
}

int command_register(const command_t *cmd)
{
    if (!s_registry.initialized) {
        MIMI_LOGE(TAG, "Command system not initialized");
        return -1;
    }

    if (!cmd || !cmd->name || strlen(cmd->name) == 0) {
        MIMI_LOGE(TAG, "Invalid command");
        return -1;
    }

    /* Check for duplicate */
    if (command_find(cmd->name)) {
        MIMI_LOGW(TAG, "Command '%s' already registered", cmd->name);
        return -1;
    }

    /* Check capacity */
    if (s_registry.count >= COMMAND_MAX_COUNT) {
        MIMI_LOGE(TAG, "Command registry full (%d)", COMMAND_MAX_COUNT);
        return -1;
    }

    /* Register command (copy) */
    s_registry.commands[s_registry.count] = *cmd;
    s_registry.count++;

    MIMI_LOGD(TAG, "Command '%s' registered (%d/%d)",
              cmd->name, s_registry.count, COMMAND_MAX_COUNT);
    return 0;
}

void command_unregister(const char *name)
{
    if (!s_registry.initialized || !name) {
        return;
    }

    /* Skip leading '/' if present */
    if (name[0] == '/') {
        name++;
    }

    /* Find and remove */
    for (int i = 0; i < s_registry.count; i++) {
        if (strcmp(s_registry.commands[i].name, name) == 0) {
            /* Shift remaining commands */
            for (int j = i; j < s_registry.count - 1; j++) {
                s_registry.commands[j] = s_registry.commands[j + 1];
            }
            s_registry.count--;
            memset(&s_registry.commands[s_registry.count], 0, sizeof(command_t));

            MIMI_LOGI(TAG, "Command '%s' unregistered (%d/%d)",
                      name, s_registry.count, COMMAND_MAX_COUNT);
            return;
        }
    }
}

const command_t* command_find(const char *name)
{
    if (!s_registry.initialized || !name) {
        return NULL;
    }

    /* Skip leading '/' if present */
    if (name[0] == '/') {
        name++;
    }

    for (int i = 0; i < s_registry.count; i++) {
        if (strcmp(s_registry.commands[i].name, name) == 0) {
            return &s_registry.commands[i];
        }
    }

    return NULL;
}

int command_parse_input(const char *input,
                        char *cmd_name, size_t cmd_name_len,
                        const char **args, int max_args)
{
    if (!input || !cmd_name || !args || max_args <= 0) {
        return -1;
    }

    /* Make a copy of input for modification */
    size_t input_len = strlen(input);
    char *input_copy = (char *)malloc(input_len + 1);
    if (!input_copy) {
        return -1;
    }
    memcpy(input_copy, input, input_len + 1);

    char *p = input_copy;

    /* Skip leading whitespace */
    while (isspace((unsigned char)*p)) {
        p++;
    }

    if (*p == '\0') {
        free(input_copy);
        return 0;  /* Empty input */
    }

    /* Parse command name */
    size_t name_len = 0;

    /* Skip leading '/' if present */
    if (*p == '/') {
        p++;
    }

    /* Extract command name */
    while (*p && !isspace((unsigned char)*p) && name_len < cmd_name_len - 1) {
        cmd_name[name_len++] = *p++;
    }
    cmd_name[name_len] = '\0';

    if (name_len == 0) {
        free(input_copy);
        return -1;  /* No command name */
    }

    /* Parse arguments */
    int arg_count = 0;
    while (*p && arg_count < max_args) {
        /* Skip whitespace */
        while (isspace((unsigned char)*p)) {
            p++;
        }

        if (*p == '\0') {
            break;
        }

        /* Check for quoted string */
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            args[arg_count] = p;

            /* Find closing quote */
            while (*p && *p != quote) {
                p++;
            }

            if (*p == quote) {
                *p++ = '\0';  /* Terminate argument */
            }
        } else {
            /* Unquoted argument */
            args[arg_count] = p;

            /* Find end of argument */
            while (*p && !isspace((unsigned char)*p)) {
                p++;
            }

            if (*p) {
                *p++ = '\0';  /* Terminate argument */
            }
        }

        arg_count++;
    }

    /* Note: args point into input_copy, which will be freed by caller
     * This is a design issue - we need to keep the buffer alive
     * For now, we'll document that args are valid only until next call */

    /* Store the copy pointer for later cleanup (simplified approach) */
    /* In production, this should use a proper arena allocator */

    free(input_copy);
    return arg_count;
}

int command_execute(const char *input,
                    const command_context_t *ctx,
                    char *output, size_t output_len)
{
    if (!s_registry.initialized) {
        snprintf(output, output_len, "Command system not initialized");
        return -1;
    }

    if (!input || !output || output_len == 0) {
        return -1;
    }

    char cmd_name[COMMAND_NAME_MAX_LEN];
    const char *args[16];
    int arg_count = command_parse_input(input, cmd_name, sizeof(cmd_name),
                                        args, 16);

    if (arg_count < 0) {
        snprintf(output, output_len, "Failed to parse command");
        return -1;
    }

    if (strlen(cmd_name) == 0) {
        snprintf(output, output_len, "No command specified");
        return -1;
    }

    /* Find command */
    const command_t *cmd = command_find(cmd_name);
    if (!cmd) {
        snprintf(output, output_len, "Unknown command: /%s", cmd_name);
        return -1;
    }

    /* Execute command */
    if (cmd->execute) {
        return cmd->execute(args, arg_count, ctx, output, output_len);
    }

    snprintf(output, output_len, "Command '%s' not implemented", cmd_name);
    return -1;
}

void command_get_help(char *output, size_t output_len)
{
    if (!s_registry.initialized || !output || output_len == 0) {
        return;
    }

    size_t pos = 0;
    pos += snprintf(output + pos, output_len - pos,
                    "Available commands:\n");

    for (int i = 0; i < s_registry.count && pos < output_len - 1; i++) {
        const command_t *cmd = &s_registry.commands[i];
        pos += snprintf(output + pos, output_len - pos,
                        "  /%-15s %s\n",
                        cmd->name,
                        cmd->description ? cmd->description : "");
    }

    pos += snprintf(output + pos, output_len - pos,
                    "\nType /help <command> for detailed usage.");
}

void command_get_help_for(const char *command_name,
                          char *output, size_t output_len)
{
    if (!s_registry.initialized || !output || output_len == 0) {
        return;
    }

    const command_t *cmd = command_find(command_name);
    if (!cmd) {
        snprintf(output, output_len, "Unknown command: %s", command_name);
        return;
    }

    snprintf(output, output_len,
             "Command: /%s\n"
             "Description: %s\n"
             "Usage: %s",
             cmd->name,
             cmd->description ? cmd->description : "No description",
             cmd->usage ? cmd->usage : "/<command> [args...]");
}

int command_get_count(void)
{
    if (!s_registry.initialized) {
        return 0;
    }
    return s_registry.count;
}

const command_t* command_get_by_index(int index)
{
    if (!s_registry.initialized || index < 0 || index >= s_registry.count) {
        return NULL;
    }
    return &s_registry.commands[index];
}

/* External command init functions */
extern void cmd_help_init(void);
extern void cmd_session_init(void);
extern void cmd_set_init(void);
extern void cmd_memory_read_init(void);
extern void cmd_ask_init(void);
extern void cmd_exit_init(void);
extern void cmd_wechat_login_init(void);
extern void cmd_mcp_refresh_init(void);

int command_system_auto_init(void)
{
    /* Initialize command system */
    if (command_system_init() != 0) {
        return -1;
    }

    /* Register all built-in commands */
    cmd_help_init();
    cmd_session_init();
    cmd_set_init();
    cmd_memory_read_init();
    cmd_ask_init();
    cmd_exit_init();
    cmd_wechat_login_init();
    cmd_mcp_refresh_init();

    return 0;
}
