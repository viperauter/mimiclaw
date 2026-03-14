#include "../fs.h"
#include "../../log.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

/* Windows compatibility */
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

/* POSIX-specific file handles */
typedef struct posix_file {
    FILE *fp;
} posix_file_t;

typedef struct posix_dir {
    DIR *dir;
} posix_dir_t;

/* -------------------------------------------------------------------------
 * Error mapping
 * ------------------------------------------------------------------------- */

static mimi_err_t map_errno_to_mimi_err(int err)
{
    if (err == ENOENT) return MIMI_ERR_NOT_FOUND;
    if (err == EACCES) return MIMI_ERR_IO;
    return MIMI_ERR_IO;
}

/* -------------------------------------------------------------------------
 * POSIX file operations
 * ------------------------------------------------------------------------- */

static mimi_err_t posix_open(void *ctx, const char *path,
                           const char *mode, void **out_handle)
{
    (void)ctx;  /* POSIX implementation doesn't need ctx */

    FILE *fp = fopen(path, mode);
    if (!fp) {
        return map_errno_to_mimi_err(errno);
    }

    posix_file_t *file = (posix_file_t *)calloc(1, sizeof(posix_file_t));
    if (!file) {
        fclose(fp);
        return MIMI_ERR_NO_MEM;
    }

    file->fp = fp;
    *out_handle = file;
    return MIMI_OK;
}

static mimi_err_t posix_read(void *handle, void *buf, size_t max_len, size_t *out_len)
{
    posix_file_t *file = (posix_file_t *)handle;
    if (!file || !file->fp) return MIMI_ERR_INVALID_ARG;

    size_t n = fread(buf, 1, max_len, file->fp);
    if (out_len) *out_len = n;

    if (n == 0 && ferror(file->fp)) {
        return MIMI_ERR_IO;
    }

    return MIMI_OK;
}

static mimi_err_t posix_write(void *handle, const void *buf, size_t len, size_t *out_written)
{
    posix_file_t *file = (posix_file_t *)handle;
    if (!file || !file->fp) return MIMI_ERR_INVALID_ARG;

    size_t n = fwrite(buf, 1, len, file->fp);
    if (out_written) *out_written = n;

    if (n != len) return MIMI_ERR_IO;
    return MIMI_OK;
}

static mimi_err_t posix_close(void *handle)
{
    posix_file_t *file = (posix_file_t *)handle;
    if (!file) return MIMI_ERR_INVALID_ARG;

    if (file->fp) fclose(file->fp);
    free(file);
    return MIMI_OK;
}

static mimi_err_t posix_seek(void *handle, long offset, int whence)
{
    posix_file_t *file = (posix_file_t *)handle;
    if (!file || !file->fp) return MIMI_ERR_INVALID_ARG;

    if (fseek(file->fp, offset, whence) != 0) return MIMI_ERR_IO;
    return MIMI_OK;
}

static mimi_err_t posix_tell(void *handle, long *out_pos)
{
    posix_file_t *file = (posix_file_t *)handle;
    if (!file || !file->fp) return MIMI_ERR_INVALID_ARG;

    long p = ftell(file->fp);
    if (p < 0) return MIMI_ERR_IO;
    *out_pos = p;
    return MIMI_OK;
}

static mimi_err_t posix_read_line(void *handle, char *buf, size_t buf_size, bool *out_eof)
{
    posix_file_t *file = (posix_file_t *)handle;
    if (!file || !file->fp) return MIMI_ERR_INVALID_ARG;

    if (out_eof) *out_eof = false;
    char *r = fgets(buf, (int)buf_size, file->fp);
    if (!r) {
        if (feof(file->fp)) {
            if (out_eof) *out_eof = true;
            buf[0] = '\0';
            return MIMI_OK;
        }
        return MIMI_ERR_IO;
    }
    return MIMI_OK;
}

/* -------------------------------------------------------------------------
 * POSIX directory and file operations
 * ------------------------------------------------------------------------- */

static mimi_err_t posix_remove(void *ctx, const char *path)
{
    (void)ctx;  /* POSIX implementation doesn't need ctx */

    if (remove(path) == 0) return MIMI_OK;
    return map_errno_to_mimi_err(errno);
}

static int mkdir_p_internal(const char *dir)
{
    if (!dir || dir[0] == '\0') return -1;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);

    /* Strip trailing slash */
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
        len--;
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static mimi_err_t posix_mkdir_p(void *ctx, const char *dir)
{
    (void)ctx;  /* POSIX implementation doesn't need ctx */

    if (!dir || dir[0] == '\0') return MIMI_ERR_INVALID_ARG;

    if (mkdir_p_internal(dir) != 0) return MIMI_ERR_IO;
    return MIMI_OK;
}

static mimi_err_t posix_rename(void *ctx, const char *old_path, const char *new_path)
{
    (void)ctx;  /* POSIX implementation doesn't need ctx */

    if (rename(old_path, new_path) == 0) return MIMI_OK;
    return map_errno_to_mimi_err(errno);
}

static mimi_err_t posix_opendir(void *ctx, const char *path, void **out_handle)
{
    (void)ctx;  /* POSIX implementation doesn't need ctx */

    DIR *d = opendir(path);
    if (!d) return map_errno_to_mimi_err(errno);

    posix_dir_t *dir = (posix_dir_t *)calloc(1, sizeof(posix_dir_t));
    if (!dir) {
        closedir(d);
        return MIMI_ERR_NO_MEM;
    }

    dir->dir = d;
    *out_handle = dir;
    return MIMI_OK;
}

static mimi_err_t posix_readdir(void *handle, char *name_out, size_t name_out_len, bool *out_has_entry)
{
    posix_dir_t *dir = (posix_dir_t *)handle;
    if (!dir || !dir->dir) return MIMI_ERR_INVALID_ARG;

    errno = 0;
    struct dirent *ent = readdir(dir->dir);
    if (!ent) {
        if (errno != 0) return MIMI_ERR_IO;
        *out_has_entry = false;
        name_out[0] = '\0';
        return MIMI_OK;
    }
    snprintf(name_out, name_out_len, "%s", ent->d_name);
    *out_has_entry = true;
    return MIMI_OK;
}

static mimi_err_t posix_closedir(void *handle)
{
    posix_dir_t *dir = (posix_dir_t *)handle;
    if (!dir) return MIMI_ERR_INVALID_ARG;

    if (dir->dir) closedir(dir->dir);
    free(dir);
    return MIMI_OK;
}

static mimi_err_t posix_exists(void *ctx, const char *path, bool *out_exists)
{
    (void)ctx;  /* POSIX implementation doesn't need ctx */

    struct stat st;
    if (stat(path, &st) == 0) {
        *out_exists = true;
        return MIMI_OK;
    }
    if (errno == ENOENT) {
        *out_exists = false;
        return MIMI_OK;
    }
    return MIMI_ERR_IO;
}

/* -------------------------------------------------------------------------
 * POSIX operations table
 * ------------------------------------------------------------------------- */

static const fs_operations_t posix_ops = {
    .open = posix_open,
    .read = posix_read,
    .write = posix_write,
    .close = posix_close,
    .seek = posix_seek,
    .tell = posix_tell,
    .read_line = posix_read_line,
    .remove = posix_remove,
    .mkdir_p = posix_mkdir_p,
    .rename = posix_rename,
    .opendir = posix_opendir,
    .readdir = posix_readdir,
    .closedir = posix_closedir,
    .exists = posix_exists,
};

/* -------------------------------------------------------------------------
 * Register POSIX implementation
 * ------------------------------------------------------------------------- */

void posix_fs_register(void)
{
    mimi_fs_register_impl(FS_TYPE_POSIX, &posix_ops);
}