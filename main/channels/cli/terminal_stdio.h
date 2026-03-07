/**
 * STDIO CLI terminal implementation
 * 
 * Moved from main/cli/ to main/channels/cli/ as part of Channel architecture refactor
 */

#ifndef CLI_TERMINAL_STDIO_H
#define CLI_TERMINAL_STDIO_H

#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the STDIO CLI terminal
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t stdio_cli_start(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_TERMINAL_STDIO_H */
