#pragma once

#include <stdbool.h>
#include <stdarg.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIMI_LOG_NONE = -1,
    MIMI_LOG_ERROR = 0,
    MIMI_LOG_WARN = 1,
    MIMI_LOG_INFO = 2,
    MIMI_LOG_DEBUG = 3,
} mimi_log_level_t;

void mimi_log(mimi_log_level_t level, const char *tag, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

void mimi_vlog(mimi_log_level_t level, const char *tag, const char *file, int line, const char *fmt, va_list ap);

/* Set log level */
void mimi_log_set_level(mimi_log_level_t level);

/* Setup log level from string */
void mimi_log_setup(const char *level_str);

/* Route logs to a file. If also_stderr is true, keep stderr output too. */
mimi_err_t mimi_log_set_output_file(const char *path, bool also_stderr);

/* Configure file rotation. max_file_bytes<=0 disables rotate checks. */
void mimi_log_set_rotation(int max_file_bytes, int max_files);

/* Close file logger; late logs are routed based on current toStderr config. */
void mimi_log_close_output_file(void);

/* Check if logging is enabled */
bool mimi_log_is_enabled(void);

/* Get current log level */
mimi_log_level_t mimi_log_get_level(void);

/* TTY sync: call before/after writing to stdout/stderr from a non-CLI thread to
 * avoid corrupting the linenoise prompt. No-op when not in stdio CLI mode. */
void mimi_tty_hide_prompt(void);
void mimi_tty_show_prompt(void);

/* Printf to stdout with automatic TTY sync. Use for agent/CLI output that may
 * interleave with the interactive prompt. */
void mimi_tty_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define MIMI_LOGE(tag, fmt, ...) mimi_log(MIMI_LOG_ERROR, (tag), __FILE__, __LINE__, (fmt), ##__VA_ARGS__)
#define MIMI_LOGW(tag, fmt, ...) mimi_log(MIMI_LOG_WARN, (tag), __FILE__, __LINE__, (fmt), ##__VA_ARGS__)
#define MIMI_LOGI(tag, fmt, ...) mimi_log(MIMI_LOG_INFO, (tag), __FILE__, __LINE__, (fmt), ##__VA_ARGS__)
#define MIMI_LOGD(tag, fmt, ...) mimi_log(MIMI_LOG_DEBUG, (tag), __FILE__, __LINE__, (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

