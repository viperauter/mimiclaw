#include "platform/fs.h"
#include "platform/log.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static char s_base[256] = {0};

mimi_err_t mimi_fs_set_base(const char *base_dir)
{
    if (!base_dir || base_dir[0] == '\0') return MIMI_ERR_INVALID_ARG;
    snprintf(s_base, sizeof(s_base), "%s", base_dir);
    return MIMI_OK;
}

mimi_err_t mimi_fs_resolve_path(const char *path_in, char *path_out, size_t path_out_len)
{
    if (!path_in || !path_out || path_out_len == 0) return MIMI_ERR_INVALID_ARG;

    /* If no base configured, use the provided path. */
    if (s_base[0] == '\0') {
        snprintf(path_out, path_out_len, "%s", path_in);
        return MIMI_OK;
    }

    /* Treat absolute paths as virtual paths rooted at base_dir. */
    if (path_in[0] == '/') {
        /* On ESP32, paths are virtual and start with "/spiffs/...".
           On POSIX, we map that virtual prefix into the configured base dir.
           Otherwise we'd get "base_dir/spiffs/..." (double spiffs) when base_dir itself is "./spiffs". */
        const char *p = path_in;
        const char *virt_root = "/spiffs";
        size_t virt_root_len = strlen(virt_root);
        if (strncmp(path_in, virt_root, virt_root_len) == 0) {
            p = path_in + virt_root_len;
            if (*p == '\0') p = "/";     /* "/spiffs" maps to base root */
        }

        /* Avoid duplicate slashes when joining. */
        if (s_base[strlen(s_base) - 1] == '/') {
            snprintf(path_out, path_out_len, "%s%s", s_base, (*p == '/') ? (p + 1) : p);
        } else {
            snprintf(path_out, path_out_len, "%s%s", s_base, p);
        }
        return MIMI_OK;
    }

    /* Relative paths are relative to base_dir. */
    if (s_base[strlen(s_base) - 1] == '/') {
        snprintf(path_out, path_out_len, "%s%s", s_base, path_in);
    } else {
        snprintf(path_out, path_out_len, "%s/%s", s_base, path_in);
    }
    return MIMI_OK;
}

