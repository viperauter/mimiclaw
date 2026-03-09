#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static mimi_log_level_t s_log_level = MIMI_LOG_NONE;

/* For linenoise compatibility - now using microrl which doesn't need this */
static int g_prompt_hidden = 0;

static const char *level_name(mimi_log_level_t level)
{
    switch (level) {
        case MIMI_LOG_ERROR: return "E";
        case MIMI_LOG_WARN: return "W";
        case MIMI_LOG_INFO: return "I";
        case MIMI_LOG_DEBUG: return "D";
        default: return "?";
    }
}

void mimi_vlog(mimi_log_level_t level, const char *tag, const char *fmt, va_list ap)
{
    if (level > s_log_level) return;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    struct tm *tm_info = localtime((const time_t *)&tv.tv_sec);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
    long ms = tv.tv_usec / 1000;

    mimi_tty_hide_prompt();
    fprintf(stderr, "%s.%03ld %s/%s: ", time_buf, ms, level_name(level), tag ? tag : "-");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    mimi_tty_show_prompt();
}

void mimi_log(mimi_log_level_t level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mimi_vlog(level, tag, fmt, ap);
    va_end(ap);
}

void mimi_log_set_level(mimi_log_level_t level)
{
    s_log_level = level;
}

void mimi_log_setup(const char *level_str)
{
    mimi_log_level_t log_level = MIMI_LOG_INFO;
    if (strcmp(level_str, "error") == 0) {
        log_level = MIMI_LOG_ERROR;
    } else if (strcmp(level_str, "warn") == 0) {
        log_level = MIMI_LOG_WARN;
    } else if (strcmp(level_str, "info") == 0) {
        log_level = MIMI_LOG_INFO;
    } else if (strcmp(level_str, "debug") == 0) {
        log_level = MIMI_LOG_DEBUG;
    }
    mimi_log_set_level(log_level);
}

bool mimi_log_is_enabled(void)
{
    return s_log_level >= 0;
}

mimi_log_level_t mimi_log_get_level(void)
{
    return s_log_level;
}

void mimi_tty_hide_prompt(void)
{
    /* microrl doesn't support hide/show prompt like linenoise */
    g_prompt_hidden = 1;
}

void mimi_tty_show_prompt(void)
{
    /* microrl doesn't support hide/show prompt like linenoise */
    g_prompt_hidden = 0;
}

void mimi_tty_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mimi_tty_hide_prompt();
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    mimi_tty_show_prompt();
}

