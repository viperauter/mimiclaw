#include "memory_store.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "platform/log.h"
#include "platform/fs.h"

static const char *TAG = "memory";

static void get_date_str(char *buf, size_t size, int days_ago)
{
    time_t now;
    time(&now);
    now -= days_ago * 86400;
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, size, "%Y-%m-%d", &tm);
}

mimi_err_t memory_store_init(void)
{
    /* SPIFFS is flat — no real directory creation needed.
       Just verify we can open the base path. */
    MIMI_LOGI(TAG, "Memory store initialized at %s", MIMI_SPIFFS_BASE);
    return MIMI_OK;
}

mimi_err_t memory_read_long_term(char *buf, size_t size)
{
    char path[256];
    mimi_fs_resolve_path(MIMI_MEMORY_FILE, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        buf[0] = '\0';
        return MIMI_ERR_NOT_FOUND;
    }

    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return MIMI_OK;
}

mimi_err_t memory_write_long_term(const char *content)
{
    char path[256];
    mimi_fs_resolve_path(MIMI_MEMORY_FILE, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        MIMI_LOGE(TAG, "Cannot write %s", path);
        return MIMI_ERR_IO;
    }
    fputs(content, f);
    fclose(f);
    MIMI_LOGI(TAG, "Long-term memory updated (%d bytes)", (int)strlen(content));
    return MIMI_OK;
}

mimi_err_t memory_append_today(const char *note)
{
    char date_str[16];
    get_date_str(date_str, sizeof(date_str), 0);

    char virt[128];
    snprintf(virt, sizeof(virt), "%s/%s.md", MIMI_SPIFFS_MEMORY_DIR, date_str);
    char path[256];
    mimi_fs_resolve_path(virt, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        /* Try creating — if file doesn't exist yet, write header */
        f = fopen(path, "w");
        if (!f) {
            MIMI_LOGE(TAG, "Cannot open %s", path);
            return MIMI_ERR_IO;
        }
        fprintf(f, "# %s\n\n", date_str);
    }

    fprintf(f, "%s\n", note);
    fclose(f);
    return MIMI_OK;
}

mimi_err_t memory_read_recent(char *buf, size_t size, int days)
{
    size_t offset = 0;
    buf[0] = '\0';

    for (int i = 0; i < days && offset < size - 1; i++) {
        char date_str[16];
        get_date_str(date_str, sizeof(date_str), i);

        char virt[128];
        snprintf(virt, sizeof(virt), "%s/%s.md", MIMI_SPIFFS_MEMORY_DIR, date_str);
        char path[256];
        mimi_fs_resolve_path(virt, path, sizeof(path));

        FILE *f = fopen(path, "r");
        if (!f) continue;

        if (offset > 0 && offset < size - 4) {
            offset += snprintf(buf + offset, size - offset, "\n---\n");
        }

        size_t n = fread(buf + offset, 1, size - offset - 1, f);
        offset += n;
        buf[offset] = '\0';
        fclose(f);
    }

    return MIMI_OK;
}
