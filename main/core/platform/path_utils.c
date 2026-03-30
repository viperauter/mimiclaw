#include "path_utils.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int mimi_path_join(const char *base, const char *component, char *out, size_t out_size)
{
    if (!base || !component || !out || out_size == 0) {
        return -1;
    }

    size_t base_len = strlen(base);
    size_t comp_len = strlen(component);

    /* Check if base already ends with separator */
    bool base_has_sep = (base_len > 0 && 
                         (base[base_len - 1] == '/' || base[base_len - 1] == '\\'));
    bool comp_has_sep = (comp_len > 0 && 
                         (component[0] == '/' || component[0] == '\\'));

    size_t total_len = base_len + comp_len + 1; /* +1 for separator */
    if (base_has_sep || comp_has_sep) {
        total_len--; /* No need for extra separator */
    }

    if (total_len >= out_size) {
        return -1; /* Buffer too small */
    }

    if (base_has_sep || comp_has_sep) {
        snprintf(out, out_size, "%s%s", base, component);
    } else {
        snprintf(out, out_size, "%s%s%s", base, MIMI_PATH_SEP, component);
    }

    return 0;
}

int mimi_path_join_multi(char *out, size_t out_size, const char *first, ...)
{
    if (!out || out_size == 0 || !first) {
        return -1;
    }

    va_list args;
    va_start(args, first);

    /* Copy first component */
    strncpy(out, first, out_size - 1);
    out[out_size - 1] = '\0';
    size_t current_len = strlen(out);

    const char *component;
    while ((component = va_arg(args, const char *)) != NULL) {
        size_t comp_len = strlen(component);
        if (comp_len == 0) {
            continue;
        }

        /* Check if we need a separator */
        bool need_sep = (current_len > 0 && 
                         out[current_len - 1] != '/' && 
                         out[current_len - 1] != '\\');
        bool comp_has_sep = (component[0] == '/' || component[0] == '\\');

        size_t add_len = comp_len;
        if (need_sep && !comp_has_sep) {
            add_len++; /* For separator */
        }

        if (current_len + add_len >= out_size) {
            va_end(args);
            return -1; /* Buffer too small */
        }

        if (need_sep && !comp_has_sep) {
            strncat(out, MIMI_PATH_SEP, out_size - current_len - 1);
            current_len++;
        }
        strncat(out, component, out_size - current_len - 1);
        current_len += comp_len;
    }

    va_end(args);
    return 0;
}

int mimi_path_dirname(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) {
        return -1;
    }

    /* Find last separator (either / or \) */
    const char *last_slash = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    const char *sep = last_slash;
    if (last_backslash && (!sep || last_backslash > sep)) {
        sep = last_backslash;
    }

    if (!sep) {
        return -1; /* No separator found */
    }

    size_t len = (size_t)(sep - path);
    if (len >= out_size) {
        return -1; /* Buffer too small */
    }

    memcpy(out, path, len);
    out[len] = '\0';
    return 0;
}

const char *mimi_path_basename(const char *path)
{
    if (!path || path[0] == '\0') {
        return ".";
    }

    /* Find last separator (either / or \) */
    const char *last_slash = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    const char *base = last_slash;
    if (last_backslash && (!base || last_backslash > base)) {
        base = last_backslash;
    }

    if (base) {
        return base + 1;
    }
    return path;
}

int mimi_path_normalize(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) {
        return -1;
    }

    size_t j = 0;
    bool last_was_sep = false;

    for (size_t i = 0; path[i] != '\0' && j < out_size - 1; i++) {
        char c = path[i];

        /* Convert any separator to platform-specific one */
        if (c == '/' || c == '\\') {
            if (!last_was_sep) {
                out[j++] = MIMI_PATH_SEP_CHAR;
                last_was_sep = true;
            }
            /* Skip consecutive separators */
        } else {
            out[j++] = c;
            last_was_sep = false;
        }
    }

    out[j] = '\0';
    return 0;
}

int mimi_path_expand_tilde(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) return -1;

    /* Only expand the common "~/" form (shell-like). */
    if (!(path[0] == '~' && path[1] == '/')) {
        size_t n = strnlen(path, out_size - 1);
        memcpy(out, path, n);
        out[n] = '\0';
        return (path[n] == '\0') ? 0 : -1;
    }

    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || home[0] == '\0') home = getenv("USERPROFILE");
#endif
    if (!home || home[0] == '\0') {
        /* No home directory available; keep the path unchanged. */
        size_t n = strnlen(path, out_size - 1);
        memcpy(out, path, n);
        out[n] = '\0';
        return (path[n] == '\0') ? 0 : -1;
    }

    /* Join as: <home>/<rest-after-~/> */
    int written = snprintf(out, out_size, "%s/%s", home, path + 2);
    return (written >= 0 && (size_t)written < out_size) ? 0 : -1;
}

int mimi_path_canonicalize(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) return -1;

    char expanded[512];
    if (mimi_path_expand_tilde(path, expanded, sizeof(expanded)) != 0) {
        return -1;
    }

    /* Normalize separators to platform-specific style. */
    if (mimi_path_normalize(expanded, out, out_size) != 0) {
        return -1;
    }
    return 0;
}

bool mimi_path_is_absolute(const char *path)
{
    if (!path || path[0] == '\0') {
        return false;
    }

#ifdef _WIN32
    /* Windows: absolute if starts with X:\ or \\ (UNC) */
    if (path[0] == '\\' && path[1] == '\\') {
        return true;
    }
    if (path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    return false;
#else
    /* Unix: absolute if starts with / */
    return path[0] == '/';
#endif
}

const char *mimi_path_extension(const char *path)
{
    if (!path) {
        return NULL;
    }

    const char *base = mimi_path_basename(path);
    const char *dot = strrchr(base, '.');

    if (!dot || dot == base) {
        return NULL; /* No extension or hidden file */
    }

    return dot;
}
