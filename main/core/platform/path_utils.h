#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-specific path separator */
#ifdef _WIN32
#define MIMI_PATH_SEP "\\"
#define MIMI_PATH_SEP_CHAR '\\'
#else
#define MIMI_PATH_SEP "/"
#define MIMI_PATH_SEP_CHAR '/'
#endif

/**
 * Join two path components with the platform-specific separator.
 * Result is written to 'out', which must have space for 'out_size' bytes.
 * Returns 0 on success, -1 on error (buffer too small).
 */
int mimi_path_join(const char *base, const char *component, char *out, size_t out_size);

/**
 * Join multiple path components (variadic version).
 * Last argument must be NULL.
 * Returns 0 on success, -1 on error.
 */
int mimi_path_join_multi(char *out, size_t out_size, const char *first, ...);

/**
 * Get the directory part of a path (everything before the last separator).
 * Result is written to 'out', which must have space for 'out_size' bytes.
 * Returns 0 on success, -1 if no separator found.
 */
int mimi_path_dirname(const char *path, char *out, size_t out_size);

/**
 * Get the filename part of a path (everything after the last separator).
 * Returns pointer to filename within the original path (not a copy).
 */
const char *mimi_path_basename(const char *path);

/**
 * Normalize a path by converting all separators to the platform-specific one.
 * Also removes redundant separators (e.g., "//" -> "/", or "/\\" -> "\")
 * Result is written to 'out', which must have space for 'out_size' bytes.
 * Returns 0 on success, -1 on error.
 */
int mimi_path_normalize(const char *path, char *out, size_t out_size);

/**
 * Check if a path is absolute.
 * Returns true if absolute, false if relative.
 */
bool mimi_path_is_absolute(const char *path);

/**
 * Get the file extension from a path.
 * Returns pointer to extension (including the dot), or NULL if no extension.
 */
const char *mimi_path_extension(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PATH_UTILS_H */
