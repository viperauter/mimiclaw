#include "platform/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ms = ts.tv_nsec / 1000000L;

    fprintf(stderr, "%ld.%03ld %s/%s: ", (long)ts.tv_sec, ms, level_name(level), tag ? tag : "-");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void mimi_log(mimi_log_level_t level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mimi_vlog(level, tag, fmt, ap);
    va_end(ap);
}

