#include "memory_store.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include "log.h"
#include "os/os.h"
#include "fs/fs.h"
#include "path_utils.h"

static const char *TAG = "memory";

static void get_date_str(char *buf, size_t size, int days_ago)
{
    uint64_t now_ms = mimi_time_ms();
    uint64_t days = now_ms / (1000ULL * 86400ULL);
    if (days_ago > 0 && (uint64_t)days_ago <= days) {
        days -= (uint64_t)days_ago;
    }
    snprintf(buf, size, "day-%llu", (unsigned long long)days);
}

mimi_err_t memory_store_init(void)
{
    /* SPIFFS is flat — no real directory creation needed.
       On POSIX we rely on workspace_bootstrap to create needed directories. */
    const mimi_config_t *cfg = mimi_config_get();
    MIMI_LOGD(TAG, "Memory store initialized (workspace=%s)",
              cfg->workspace[0] ? cfg->workspace : "(default)");
    return MIMI_OK;
}

mimi_err_t memory_read_long_term(char *buf, size_t size)
{
    const mimi_config_t *cfg = mimi_config_get();
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(cfg->memory_file, "r", &f);
    if (err != MIMI_OK) {
        buf[0] = '\0';
        return err;
    }

    size_t n = 0;
    err = mimi_fs_read(f, buf, size - 1, &n);
    buf[n] = '\0';
    mimi_fs_close(f);
    return err;
}

mimi_err_t memory_write_long_term(const char *content)
{
    const mimi_config_t *cfg = mimi_config_get();
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(cfg->memory_file, "w", &f);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Cannot write %s", cfg->memory_file);
        return err;
    }
    size_t written = 0;
    err = mimi_fs_write(f, content, strlen(content), &written);
    mimi_fs_close(f);
    if (err != MIMI_OK) return err;
    MIMI_LOGI(TAG, "Long-term memory updated (%d bytes)", (int)strlen(content));
    return MIMI_OK;
}

mimi_err_t memory_append_today(const char *note)
{
    char date_str[16];
    get_date_str(date_str, sizeof(date_str), 0);

    const mimi_config_t *cfg = mimi_config_get();

    /* Derive daily notes directory from memory_file dirname. */
    char base[256];
    if (mimi_path_dirname(cfg->memory_file, base, sizeof(base)) != 0) {
        strncpy(base, "memory", sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }

    char path[512];
    mimi_path_join_multi(path, sizeof(path), base, "daily", date_str, ".md", NULL);

    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(path, "a", &f);
    if (err != MIMI_OK) {
        /* Try creating — if file doesn't exist yet, write header */
        err = mimi_fs_open(path, "w", &f);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Cannot open %s", path);
            return err;
        }
        char header[64];
        int hdr_len = snprintf(header, sizeof(header), "# %s\n\n", date_str);
        size_t written = 0;
        err = mimi_fs_write(f, header, (size_t)hdr_len, &written);
        if (err != MIMI_OK) {
            mimi_fs_close(f);
            return err;
        }
    }

    size_t written = 0;
    mimi_err_t werr = mimi_fs_write(f, note, strlen(note), &written);
    if (werr == MIMI_OK) {
        /* Append trailing newline */
        const char nl = '\n';
        size_t w2 = 0;
        (void)mimi_fs_write(f, &nl, 1, &w2);
    }
    mimi_fs_close(f);
    return werr;
}

mimi_err_t memory_read_recent(char *buf, size_t size, int days)
{
    size_t offset = 0;
    buf[0] = '\0';

    for (int i = 0; i < days && offset < size - 1; i++) {
        char date_str[16];
        get_date_str(date_str, sizeof(date_str), i);

        const mimi_config_t *cfg = mimi_config_get();

        char base[256];
        if (mimi_path_dirname(cfg->memory_file, base, sizeof(base)) != 0) {
            strncpy(base, "memory", sizeof(base) - 1);
            base[sizeof(base) - 1] = '\0';
        }

        char path[512];
        mimi_path_join_multi(path, sizeof(path), base, "daily", date_str, ".md", NULL);

        mimi_file_t *f = NULL;
        mimi_err_t err = mimi_fs_open(path, "r", &f);
        if (err != MIMI_OK) continue;

        if (offset > 0 && offset < size - 4) {
            offset += snprintf(buf + offset, size - offset, "\n---\n");
        }

        size_t n = 0;
        err = mimi_fs_read(f, buf + offset, size - offset - 1, &n);
        offset += n;
        buf[offset] = '\0';
        mimi_fs_close(f);
    }

    return MIMI_OK;
}
