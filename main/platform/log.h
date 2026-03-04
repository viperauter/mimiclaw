#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIMI_LOG_ERROR = 0,
    MIMI_LOG_WARN = 1,
    MIMI_LOG_INFO = 2,
    MIMI_LOG_DEBUG = 3,
} mimi_log_level_t;

void mimi_log(mimi_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void mimi_vlog(mimi_log_level_t level, const char *tag, const char *fmt, va_list ap);

#define MIMI_LOGE(tag, fmt, ...) mimi_log(MIMI_LOG_ERROR, (tag), (fmt), ##__VA_ARGS__)
#define MIMI_LOGW(tag, fmt, ...) mimi_log(MIMI_LOG_WARN, (tag), (fmt), ##__VA_ARGS__)
#define MIMI_LOGI(tag, fmt, ...) mimi_log(MIMI_LOG_INFO, (tag), (fmt), ##__VA_ARGS__)
#define MIMI_LOGD(tag, fmt, ...) mimi_log(MIMI_LOG_DEBUG, (tag), (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

