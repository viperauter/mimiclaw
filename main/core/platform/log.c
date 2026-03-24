#include "log.h"
#include "path_utils.h"
#include "fs/fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>

static mimi_log_level_t s_log_level = MIMI_LOG_NONE;
static FILE *s_log_file = NULL;
static bool s_log_to_stderr = true;
static char s_log_file_path[1024];
static int s_rotate_max_file_bytes = 5 * 1024 * 1024; /* 5MB */
static int s_rotate_max_files = 3;

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

static long file_size_bytes(const char *path)
{
    struct stat st;
    if (!path || path[0] == '\0') return 0;
    if (stat(path, &st) != 0) return 0;
    return (long)st.st_size;
}

static void maybe_rotate_file(void)
{
    if (!s_log_file || s_log_file_path[0] == '\0') return;
    if (s_rotate_max_file_bytes <= 0 || s_rotate_max_files <= 0) return;

    long sz = file_size_bytes(s_log_file_path);
    if (sz <= 0 || sz < (long)s_rotate_max_file_bytes) return;

    fclose(s_log_file);
    s_log_file = NULL;

    for (int i = s_rotate_max_files - 1; i >= 1; --i) {
        char old_path[1200];
        char new_path[1200];
        snprintf(old_path, sizeof(old_path), "%s.%d", s_log_file_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", s_log_file_path, i + 1);
        (void)remove(new_path);
        (void)rename(old_path, new_path);
    }

    {
        char first_path[1200];
        snprintf(first_path, sizeof(first_path), "%s.1", s_log_file_path);
        (void)remove(first_path);
        (void)rename(s_log_file_path, first_path);
    }

    s_log_file = fopen(s_log_file_path, "ab");
}

void mimi_vlog(mimi_log_level_t level, const char *tag, const char *file, int line, const char *fmt, va_list ap)
{
    if (level > s_log_level) return;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    struct tm *tm_info = localtime((const time_t *)&tv.tv_sec);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);
    long ms = tv.tv_usec / 1000;

    // Extract filename from path
    const char *filename = file;
    const char *slash = strrchr(file, '/');
    if (slash) {
        filename = slash + 1;
    }

    if (s_log_to_stderr) {
        mimi_tty_hide_prompt();
        fprintf(stderr, "%s.%03ld %s tag=%s file=%s line=%d: ",
                time_buf, ms, level_name(level), tag ? tag : "-", filename, line);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        fflush(stderr);
        mimi_tty_show_prompt();
    }

    if (s_log_file) {
        maybe_rotate_file();
        if (!s_log_file) return;
        va_list ap_copy;
        va_copy(ap_copy, ap);
        fprintf(s_log_file, "%s.%03ld %s tag=%s file=%s line=%d: ",
                time_buf, ms, level_name(level), tag ? tag : "-", filename, line);
        vfprintf(s_log_file, fmt, ap_copy);
        va_end(ap_copy);
        fputc('\n', s_log_file);
        fflush(s_log_file);
    }
}

void mimi_log(mimi_log_level_t level, const char *tag, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mimi_vlog(level, tag, file, line, fmt, ap);
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

mimi_err_t mimi_log_set_output_file(const char *path, bool also_stderr)
{
    if (!path || path[0] == '\0') return MIMI_ERR_INVALID_ARG;

    char dir[512];
    if (mimi_path_dirname(path, dir, sizeof(dir)) == 0) {
        (void)mimi_fs_mkdir_p_direct(dir);
    }

    FILE *fp = fopen(path, "ab");
    if (!fp) return MIMI_ERR_IO;

    if (s_log_file) {
        fclose(s_log_file);
    }
    s_log_file = fp;
    s_log_to_stderr = also_stderr;
    strncpy(s_log_file_path, path, sizeof(s_log_file_path) - 1);
    s_log_file_path[sizeof(s_log_file_path) - 1] = '\0';
    return MIMI_OK;
}

void mimi_log_set_rotation(int max_file_bytes, int max_files)
{
    s_rotate_max_file_bytes = max_file_bytes;
    s_rotate_max_files = max_files;
}

void mimi_log_close_output_file(void)
{
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    s_log_file_path[0] = '\0';
    s_log_to_stderr = true;
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

