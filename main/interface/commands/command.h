/**
 * Command System Interface
 * 
 * Provides a unified command registration and execution framework
 * that can be shared across all channels (CLI, Telegram, WebSocket, etc.)
 */

#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum command name length */
#define COMMAND_NAME_MAX_LEN 32

/* Maximum number of registered commands */
#define COMMAND_MAX_COUNT 32

/* Maximum output buffer size for command execution */
#define COMMAND_OUTPUT_MAX_LEN 4096

/* Forward declaration */
struct command;
typedef struct command command_t;

/**
 * Command execution context
 * Passed to command execute function with channel and session info
 */
typedef struct {
    const char *channel;        /* Source channel name, e.g., "cli", "telegram" */
    const char *session_id;     /* Session identifier */
    const char *user_id;        /* User identifier */
    void *user_data;            /* Channel-specific user data */
} command_context_t;

/**
 * Command definition structure
 * Each command must define this structure and register it
 */
struct command {
    const char *name;           /* Command name without leading '/', e.g., "help" */
    const char *description;    /* Brief description of the command */
    const char *usage;          /* Usage string, e.g., "/help [command]" */
    
    /**
     * Execute the command
     * @param args Array of arguments (excluding command name)
     * @param arg_count Number of arguments
     * @param ctx Command execution context
     * @param output Output buffer for command result
     * @param output_len Size of output buffer
     * @return 0 on success, non-zero on error
     */
    int (*execute)(const char **args, int arg_count,
                   const command_context_t *ctx,
                   char *output, size_t output_len);
};

/**
 * Initialize the command system
 * Must be called before any other command functions
 * @return 0 on success, non-zero on error
 */
int command_system_init(void);

/**
 * Deinitialize the command system
 */
void command_system_deinit(void);

/**
 * Register a command
 * @param cmd Command definition to register
 * @return 0 on success, non-zero on error (e.g., duplicate name, full registry)
 */
int command_register(const command_t *cmd);

/**
 * Unregister a command
 * @param name Command name to unregister
 */
void command_unregister(const char *name);

/**
 * Find a registered command by name
 * @param name Command name (with or without leading '/')
 * @return Command definition, or NULL if not found
 */
const command_t* command_find(const char *name);

/**
 * Execute a command from input string
 * Parses the input and dispatches to the appropriate command
 * @param input Full command line input (e.g., "/session new")
 * @param ctx Command execution context
 * @param output Output buffer for command result
 * @param output_len Size of output buffer
 * @return 0 on success, non-zero on error (e.g., unknown command)
 */
int command_execute(const char *input,
                    const command_context_t *ctx,
                    char *output, size_t output_len);

/**
 * Get help text for all registered commands
 * @param output Output buffer for help text
 * @param output_len Size of output buffer
 */
void command_get_help(char *output, size_t output_len);

/**
 * Get help text for a specific command
 * @param command_name Command name (with or without leading '/')
 * @param output Output buffer for help text
 * @param output_len Size of output buffer
 */
void command_get_help_for(const char *command_name,
                          char *output, size_t output_len);

/**
 * Get number of registered commands
 * @return Number of commands
 */
int command_get_count(void);

/**
 * Get command by index
 * @param index Command index (0-based)
 * @return Command definition, or NULL if index is out of range
 */
const command_t* command_get_by_index(int index);

/**
 * Auto-initialize command system and register all built-in commands
 * This is a convenience function that calls command_system_init() and
 * registers all standard commands (help, session, set, memory, ask, exit)
 * @return 0 on success, non-zero on error
 */
int command_system_auto_init(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_H */
