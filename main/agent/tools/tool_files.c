#include "tools/tool_files.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include "log.h"
#include "cJSON.h"
#include "fs/fs.h"

static const char *TAG = "tool_files";

#define MAX_FILE_SIZE (32 * 1024)
#define MAX_RESOLVED_PATH 512

/* Try to parse tool input JSON, with a small amount of robustness for
 * slightly malformed wrappers (e.g. extra text before/after the JSON
 * object). Returns NULL only when we cannot find a JSON object.
 */
static cJSON *parse_tool_input(const char *input_json)
{
    if (!input_json) {
        MIMI_LOGI(TAG, "parse_tool_input: input_json is NULL");
        return NULL;
    }

    MIMI_LOGI(TAG, "parse_tool_input: input_json='%s'", input_json);

    /* First attempt: direct parse */
    cJSON *root = cJSON_Parse(input_json);
    if (root) {
        MIMI_LOGI(TAG, "parse_tool_input: direct parse succeeded");
        return root;
    }

    MIMI_LOGI(TAG, "parse_tool_input: direct parse failed, trying fallback");

    /* Fallback: try to locate the first '{' and last '}' and parse the slice.
     * This helps when the arguments contain extra text like
     * "arguments: {\"path\": \"...\", ...}" or logging prefixes.
     */
    const char *start = strchr(input_json, '{');
    const char *end = strrchr(input_json, '}');
    if (!start || !end || end <= start) {
        MIMI_LOGI(TAG, "parse_tool_input: fallback failed - no braces found");
        return NULL;
    }

    size_t len = (size_t)(end - start + 1);
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        MIMI_LOGI(TAG, "parse_tool_input: fallback failed - malloc failed");
        return NULL;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';

    MIMI_LOGI(TAG, "parse_tool_input: fallback JSON='%s'", buf);

    root = cJSON_Parse(buf);
    if (root) {
        MIMI_LOGI(TAG, "parse_tool_input: fallback parse succeeded");
    } else {
        MIMI_LOGI(TAG, "parse_tool_input: fallback parse failed");
    }
    free(buf);
    return root;
}

/**
 * Validate that a path is safe: must not contain "..".
 */
static bool validate_path(const char *path)
{
    if (!path) return false;
    if (strstr(path, "..") != NULL) return false;
    return true;
}

/**
 * Resolve path to per-session workspace when session_ctx has workspace_root.
 * Relative paths -> {workspace_root}/{path}
 * Paths starting with '/' -> unchanged (global/mount)
 */
static void resolve_session_path(const char *path, const mimi_session_ctx_t *session_ctx,
                                 char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) {
        if (out && out_size > 0) out[0] = '\0';
        return;
    }
    if (!session_ctx || !session_ctx->workspace_root[0] || path[0] == '/') {
        strncpy(out, path, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    snprintf(out, out_size, "%s/%s", session_ctx->workspace_root, path);
}

/* ── read_file ─────────────────────────────────────────────── */

mimi_err_t tool_read_file_execute(const char *input_json, char *output, size_t output_size,
                                  const mimi_session_ctx_t *session_ctx)
{
    cJSON *root = parse_tool_input(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (!validate_path(path)) {
        snprintf(output, output_size, "Error: path must not contain '..'");
        cJSON_Delete(root);
        return MIMI_ERR_INVALID_ARG;
    }

    char resolved[MAX_RESOLVED_PATH];
    resolve_session_path(path, session_ctx, resolved, sizeof(resolved));

    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(resolved, "r", &f);
    if (err != MIMI_OK) {
        snprintf(output, output_size, "Error: file not found: %s", path);
        cJSON_Delete(root);
        return MIMI_ERR_NOT_FOUND;
    }

    size_t max_read = output_size - 1;
    if (max_read > MAX_FILE_SIZE) max_read = MAX_FILE_SIZE;

    size_t n = 0;
    err = mimi_fs_read(f, output, max_read, &n);
    output[n] = '\0';
    mimi_fs_close(f);

    MIMI_LOGI(TAG, "read_file: %s (%d bytes)", path, (int)n);
    cJSON_Delete(root);
    return err;
}

/* ── write_file ────────────────────────────────────────────── */

mimi_err_t tool_write_file_execute(const char *input_json, char *output, size_t output_size,
                                   const mimi_session_ctx_t *session_ctx)
{
    cJSON *root = parse_tool_input(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));

    if (!validate_path(path)) {
        snprintf(output, output_size, "Error: path must not contain '..'");
        cJSON_Delete(root);
        return MIMI_ERR_INVALID_ARG;
    }
    if (!content) {
        snprintf(output, output_size, "Error: missing 'content' field");
        cJSON_Delete(root);
        return MIMI_ERR_INVALID_ARG;
    }

    char resolved[MAX_RESOLVED_PATH];
    resolve_session_path(path, session_ctx, resolved, sizeof(resolved));

    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(resolved, "w", &f);
    if (err != MIMI_OK) {
        snprintf(output, output_size, "Error: cannot open file for writing: %s", path);
        cJSON_Delete(root);
        return MIMI_ERR_IO;
    }

    size_t len = strlen(content);
    size_t written = 0;
    err = mimi_fs_write(f, content, len, &written);
    mimi_fs_close(f);

    if (err != MIMI_OK || written != len) {
        snprintf(output, output_size, "Error: wrote %d of %d bytes to %s", (int)written, (int)len, path);
        cJSON_Delete(root);
        return MIMI_ERR_IO;
    }

    snprintf(output, output_size, "OK: wrote %d bytes to %s", (int)written, path);
    MIMI_LOGI(TAG, "write_file: %s (%d bytes)", path, (int)written);
    cJSON_Delete(root);
    return MIMI_OK;
}

/* ── edit_file ─────────────────────────────────────────────── */

mimi_err_t tool_edit_file_execute(const char *input_json, char *output, size_t output_size,
                                  const mimi_session_ctx_t *session_ctx)
{
    cJSON *root = parse_tool_input(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *old_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "old_string"));
    const char *new_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "new_string"));

    if (!validate_path(path)) {
        snprintf(output, output_size, "Error: path must not contain '..'");
        cJSON_Delete(root);
        return MIMI_ERR_INVALID_ARG;
    }
    if (!old_str || !new_str) {
        snprintf(output, output_size, "Error: missing 'old_string' or 'new_string' field");
        cJSON_Delete(root);
        return MIMI_ERR_INVALID_ARG;
    }

    char resolved[MAX_RESOLVED_PATH];
    resolve_session_path(path, session_ctx, resolved, sizeof(resolved));

    /* Read existing file */
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(resolved, "r", &f);
    if (err != MIMI_OK) {
        snprintf(output, output_size, "Error: file not found: %s", path);
        cJSON_Delete(root);
        return MIMI_ERR_NOT_FOUND;
    }

    (void)mimi_fs_seek(f, 0, SEEK_END);
    long file_size = 0;
    (void)mimi_fs_tell(f, &file_size);
    (void)mimi_fs_seek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > MAX_FILE_SIZE) {
        snprintf(output, output_size, "Error: file too large or empty (%ld bytes)", file_size);
        mimi_fs_close(f);
        cJSON_Delete(root);
        return MIMI_ERR_FAIL;
    }

    /* Allocate buffer for the result (old content + possible expansion) */
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t max_result = file_size + (new_len > old_len ? new_len - old_len : 0) + 1;
    char *buf = malloc(file_size + 1);
    char *result = malloc(max_result);
    if (!buf || !result) {
        free(buf);
        free(result);
        mimi_fs_close(f);
        snprintf(output, output_size, "Error: out of memory");
        cJSON_Delete(root);
        return MIMI_ERR_NO_MEM;
    }

    size_t n = 0;
    err = mimi_fs_read(f, buf, (size_t)file_size, &n);
    buf[n] = '\0';
    mimi_fs_close(f);
    if (err != MIMI_OK) {
        free(buf);
        free(result);
        snprintf(output, output_size, "Error: read failed for %s", path);
        cJSON_Delete(root);
        return err;
    }

    /* Find and replace first occurrence */
    char *pos = strstr(buf, old_str);
    if (!pos) {
        snprintf(output, output_size, "Error: old_string not found in %s", path);
        free(buf);
        free(result);
        cJSON_Delete(root);
        return MIMI_ERR_NOT_FOUND;
    }

    size_t prefix_len = pos - buf;
    memcpy(result, buf, prefix_len);
    memcpy(result + prefix_len, new_str, new_len);
    size_t suffix_start = prefix_len + old_len;
    size_t suffix_len = n - suffix_start;
    memcpy(result + prefix_len + new_len, buf + suffix_start, suffix_len);
    size_t total = prefix_len + new_len + suffix_len;
    result[total] = '\0';

    free(buf);

    /* Write back */
    err = mimi_fs_open(resolved, "w", &f);
    if (err != MIMI_OK) {
        snprintf(output, output_size, "Error: cannot open file for writing: %s", path);
        free(result);
        cJSON_Delete(root);
        return MIMI_ERR_IO;
    }

    size_t w = 0;
    err = mimi_fs_write(f, result, total, &w);
    mimi_fs_close(f);
    free(result);
    if (err != MIMI_OK || w != total) {
        snprintf(output, output_size, "Error: failed to write updated content to %s", path);
        cJSON_Delete(root);
        return MIMI_ERR_IO;
    }

    snprintf(output, output_size, "OK: edited %s (replaced %d bytes with %d bytes)", path, (int)old_len, (int)new_len);
    MIMI_LOGI(TAG, "edit_file: %s", path);
    cJSON_Delete(root);
    return MIMI_OK;
}

/* ── list_dir ──────────────────────────────────────────────── */

mimi_err_t tool_list_dir_execute(const char *input_json, char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx)
{
    cJSON *root = parse_tool_input(input_json);
    const char *prefix = NULL;
    if (root) {
        cJSON *pfx = cJSON_GetObjectItem(root, "prefix");
        if (pfx && cJSON_IsString(pfx)) {
            prefix = pfx->valuestring;
        }
    }

    const char *list_path = ".";
    if (session_ctx && session_ctx->workspace_root[0]) {
        list_path = session_ctx->workspace_root;
    }

    mimi_dir_t *dir = NULL;
    mimi_err_t err = mimi_fs_opendir(list_path, &dir);
    if (err != MIMI_OK) {
        snprintf(output, output_size, "Error: cannot open current directory");
        cJSON_Delete(root);
        return MIMI_ERR_IO;
    }

    size_t off = 0;
    int count = 0;

    for (;;) {
        bool has = false;
        char name[256];
        err = mimi_fs_readdir(dir, name, sizeof(name), &has);
        if (err != MIMI_OK) break;
        if (!has) break;

        if (prefix && strncmp(name, prefix, strlen(prefix)) != 0) {
            continue;
        }

        off += snprintf(output + off, output_size - off, "%s\n", name);
        count++;
    }

    mimi_fs_closedir(dir);

    if (count == 0) {
        snprintf(output, output_size, "(no files found)");
    }

    MIMI_LOGI(TAG, "list_dir: %d files (prefix=%s)", count, prefix ? prefix : "(none)");
    cJSON_Delete(root);
    return (err == MIMI_OK) ? MIMI_OK : err;
}
