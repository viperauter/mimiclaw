/**
 * Command Registry
 * 
 * Internal header for command registry implementation
 */

#ifndef COMMAND_REGISTRY_H
#define COMMAND_REGISTRY_H

#include "commands/command.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check if command system is initialized
 * @return true if initialized, false otherwise
 */
bool command_system_is_initialized(void);

/**
 * Parse command input into command name and arguments
 * @param input Input string (e.g., "/session new test")
 * @param cmd_name Buffer to store command name
 * @param cmd_name_len Size of command name buffer
 * @param args Array to store argument pointers
 * @param max_args Maximum number of arguments
 * @return Number of arguments parsed, or -1 on error
 */
int command_parse_input(const char *input,
                        char *cmd_name, size_t cmd_name_len,
                        const char **args, int max_args,
                        char **out_owned_buf);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_REGISTRY_H */
