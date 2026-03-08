#include "fs.h"
#include "log.h"
#include "path_utils.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_WORKSPACES 8
#define MAX_MOUNT_POINTS 16
#define MAX_FS_IMPLS 4

static const char *TAG = "vfs";

/* ==========================================================================
 * Internal data structures
 * ========================================================================== */

/* File system implementation registry */
typedef struct fs_impl {
    fs_type_t type;
    const fs_operations_t *ops;
} fs_impl_t;

/* Workspace structure - internal representation */
typedef struct workspace {
    char name[64];
    char base_dir[512];
    fs_type_t type;
    bool read_only;
    void *impl_data;  /* Implementation-specific data */
} workspace_t;

/* Mount point mapping */
typedef struct mount_entry {
    char virt_path[64];
    char real_path[512];
    bool read_only;
} mount_entry_t;

/* File handle structure */
struct mimi_file {
    workspace_t *workspace;
    void *impl_handle;
};

/* Directory handle structure */
struct mimi_dir {
    workspace_t *workspace;
    void *impl_handle;
};

/* Global state */
static fs_impl_t s_impls[MAX_FS_IMPLS];
static int s_impl_count = 0;

static workspace_t s_workspaces[MAX_WORKSPACES];
static int s_workspace_count = 0;
static int s_current_workspace = -1;

static mount_entry_t s_mounts[MAX_MOUNT_POINTS];
static int s_mount_count = 0;

/* ==========================================================================
 * File system implementation registration
 * ========================================================================== */

mimi_err_t mimi_fs_register_impl(fs_type_t type, const fs_operations_t *ops)
{
    if (s_impl_count >= MAX_FS_IMPLS) {
        MIMI_LOGE(TAG, "Maximum number of FS implementations reached");
        return MIMI_ERR_NO_MEM;
    }

    s_impls[s_impl_count].type = type;
    s_impls[s_impl_count].ops = ops;
    s_impl_count++;

    MIMI_LOGI(TAG, "Registered FS implementation: type=%d", type);
    return MIMI_OK;
}

static const fs_operations_t *get_impl_ops(fs_type_t type)
{
    for (int i = 0; i < s_impl_count; i++) {
        if (s_impls[i].type == type) {
            return s_impls[i].ops;
        }
    }
    return NULL;
}

/* ==========================================================================
 * Path resolution
 * ========================================================================== */

/* Platform-specific path separator */
#ifdef _WIN32
#define PATH_SEP "\\"
#define PATH_SEP_CHAR '\\'
#else
#define PATH_SEP "/"
#define PATH_SEP_CHAR '/'
#endif

static mimi_err_t resolve_path(const char *virt_path, char *out_real_path, size_t size)
{
    if (!virt_path || !out_real_path || size == 0) {
        return MIMI_ERR_INVALID_ARG;
    }

    workspace_t *ws = (s_current_workspace >= 0) ? &s_workspaces[s_current_workspace] : NULL;

    /* If absolute path, check mount points first */
    if (mimi_path_is_absolute(virt_path)) {
        for (int i = 0; i < s_mount_count; i++) {
            size_t mount_len = strlen(s_mounts[i].virt_path);
            if (strncmp(virt_path, s_mounts[i].virt_path, mount_len) == 0) {
                /* Matched mount point, replace with real path */
                const char *rel_path = virt_path + mount_len;
                if (rel_path[0] == '/') rel_path++; /* Skip leading slash */
                snprintf(out_real_path, size, "%s" PATH_SEP "%s", s_mounts[i].real_path, rel_path);
                return MIMI_OK;
            }
        }
    }

    /* No mount point matched, use workspace base directory */
    if (ws && ws->base_dir[0] != '\0') {
        if (mimi_path_is_absolute(virt_path)) {
            /* Absolute path: check if it's already under base_dir */
            size_t base_len = strlen(ws->base_dir);
            if (strncmp(virt_path, ws->base_dir, base_len) == 0) {
                /* Path already starts with base_dir, use as-is */
                snprintf(out_real_path, size, "%s", virt_path);
            } else {
                /* Absolute path not under base_dir: use as-is (allow access outside workspace) */
                snprintf(out_real_path, size, "%s", virt_path);
            }
        } else {
            /* Relative path: prepend base_dir */
            snprintf(out_real_path, size, "%s" PATH_SEP "%s", ws->base_dir, virt_path);
        }
    } else {
        /* No workspace or base_dir, use path as-is */
        snprintf(out_real_path, size, "%s", virt_path);
    }

    return MIMI_OK;
}

/* ==========================================================================
 * Permission checking
 * ========================================================================== */

static bool check_permission(const char *path, bool write)
{
    workspace_t *ws = (s_current_workspace >= 0) ? &s_workspaces[s_current_workspace] : NULL;

    /* Check if workspace is read-only */
    if (write && ws && ws->read_only) {
        MIMI_LOGW(TAG, "Write operation denied on read-only workspace");
        return false;
    }

    /* Check if mount point is read-only */
    if (path && path[0] == '/') {
        for (int i = 0; i < s_mount_count; i++) {
            size_t mount_len = strlen(s_mounts[i].virt_path);
            if (strncmp(path, s_mounts[i].virt_path, mount_len) == 0) {
                if (write && s_mounts[i].read_only) {
                    MIMI_LOGW(TAG, "Write operation denied on read-only mount: %s",
                              s_mounts[i].virt_path);
                    return false;
                }
                break;
            }
        }
    }

    return true;
}

/* ==========================================================================
 * Workspace management
 * ========================================================================== */

mimi_err_t mimi_fs_workspace_create(const char *name, const char *base_dir)
{
    if (!name || !base_dir) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (s_workspace_count >= MAX_WORKSPACES) {
        MIMI_LOGE(TAG, "Maximum number of workspaces reached");
        return MIMI_ERR_NO_MEM;
    }

    /* Check if name already exists */
    for (int i = 0; i < s_workspace_count; i++) {
        if (strcmp(s_workspaces[i].name, name) == 0) {
            return MIMI_ERR_FAIL;  /* Already exists */
        }
    }

    workspace_t *ws = &s_workspaces[s_workspace_count];
    strncpy(ws->name, name, sizeof(ws->name) - 1);
    ws->name[sizeof(ws->name) - 1] = '\0';

    strncpy(ws->base_dir, base_dir, sizeof(ws->base_dir) - 1);
    ws->base_dir[sizeof(ws->base_dir) - 1] = '\0';

    /* Remove trailing slash */
    size_t len = strlen(ws->base_dir);
    while (len > 1 && ws->base_dir[len - 1] == '/') {
        ws->base_dir[len - 1] = '\0';
        len--;
    }

    ws->type = FS_TYPE_POSIX;  /* Default to POSIX */
    ws->read_only = false;
    ws->impl_data = NULL;

    s_workspace_count++;

    MIMI_LOGI(TAG, "Created workspace '%s' with base: %s", name, base_dir);
    return MIMI_OK;
}

mimi_err_t mimi_fs_workspace_delete(const char *name)
{
    if (!name) return MIMI_ERR_INVALID_ARG;

    for (int i = 0; i < s_workspace_count; i++) {
        if (strcmp(s_workspaces[i].name, name) == 0) {
            /* Cannot delete currently active workspace */
            if (s_current_workspace == i) {
                return MIMI_ERR_INVALID_STATE;
            }

            /* Compact array */
            for (int j = i; j < s_workspace_count - 1; j++) {
                s_workspaces[j] = s_workspaces[j + 1];
            }
            s_workspace_count--;

            /* Adjust current workspace index if needed */
            if (s_current_workspace > i) {
                s_current_workspace--;
            }

            MIMI_LOGI(TAG, "Deleted workspace '%s'", name);
            return MIMI_OK;
        }
    }

    return MIMI_ERR_NOT_FOUND;
}

mimi_err_t mimi_fs_workspace_activate(const char *name)
{
    if (!name) return MIMI_ERR_INVALID_ARG;

    for (int i = 0; i < s_workspace_count; i++) {
        if (strcmp(s_workspaces[i].name, name) == 0) {
            s_current_workspace = i;
            MIMI_LOGI(TAG, "Activated workspace '%s' (base: %s)",
                      name, s_workspaces[i].base_dir);
            return MIMI_OK;
        }
    }

    return MIMI_ERR_NOT_FOUND;
}

mimi_err_t mimi_fs_workspace_get_current(char *name_out, size_t name_out_len)
{
    if (!name_out || name_out_len == 0) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (s_current_workspace < 0 || s_current_workspace >= s_workspace_count) {
        return MIMI_ERR_INVALID_STATE;
    }

    strncpy(name_out, s_workspaces[s_current_workspace].name, name_out_len - 1);
    name_out[name_out_len - 1] = '\0';
    return MIMI_OK;
}

mimi_err_t mimi_fs_set_base(const char *base_dir)
{
    if (s_current_workspace < 0) {
        return MIMI_ERR_INVALID_STATE;
    }

    workspace_t *ws = &s_workspaces[s_current_workspace];

    if (base_dir) {
        strncpy(ws->base_dir, base_dir, sizeof(ws->base_dir) - 1);
        ws->base_dir[sizeof(ws->base_dir) - 1] = '\0';

        /* Remove trailing slash */
        size_t len = strlen(ws->base_dir);
        while (len > 1 && ws->base_dir[len - 1] == '/') {
            ws->base_dir[len - 1] = '\0';
            len--;
        }
    } else {
        ws->base_dir[0] = '\0';
    }

    MIMI_LOGI(TAG, "Set base directory for workspace '%s': %s",
              ws->name, base_dir ? base_dir : "(none)");
    return MIMI_OK;
}

/* ==========================================================================
 * Mount point management
 * ========================================================================== */

mimi_err_t mimi_fs_mount(const char *virt_path, const char *real_path)
{
    if (!virt_path || !real_path) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (s_mount_count >= MAX_MOUNT_POINTS) {
        MIMI_LOGE(TAG, "Maximum number of mount points reached");
        return MIMI_ERR_NO_MEM;
    }

    mount_entry_t *mount = &s_mounts[s_mount_count];

    strncpy(mount->virt_path, virt_path, sizeof(mount->virt_path) - 1);
    mount->virt_path[sizeof(mount->virt_path) - 1] = '\0';

    strncpy(mount->real_path, real_path, sizeof(mount->real_path) - 1);
    mount->real_path[sizeof(mount->real_path) - 1] = '\0';

    /* Remove trailing slash from real_path */
    size_t len = strlen(mount->real_path);
    while (len > 1 && mount->real_path[len - 1] == '/') {
        mount->real_path[len - 1] = '\0';
        len--;
    }

    mount->read_only = false;
    s_mount_count++;

    MIMI_LOGI(TAG, "Mounted '%s' -> '%s'", virt_path, real_path);
    return MIMI_OK;
}

mimi_err_t mimi_fs_unmount(const char *virt_path)
{
    if (!virt_path) return MIMI_ERR_INVALID_ARG;

    for (int i = 0; i < s_mount_count; i++) {
        if (strcmp(s_mounts[i].virt_path, virt_path) == 0) {
            /* Compact array */
            for (int j = i; j < s_mount_count - 1; j++) {
                s_mounts[j] = s_mounts[j + 1];
            }
            s_mount_count--;
            MIMI_LOGI(TAG, "Unmounted '%s'", virt_path);
            return MIMI_OK;
        }
    }

    return MIMI_ERR_NOT_FOUND;
}

/* ==========================================================================
 * Permission control
 * ========================================================================== */

mimi_err_t mimi_fs_set_readonly(const char *virt_path, bool readonly)
{
    if (!virt_path) return MIMI_ERR_INVALID_ARG;

    /* Check mount points */
    for (int i = 0; i < s_mount_count; i++) {
        if (strcmp(s_mounts[i].virt_path, virt_path) == 0) {
            s_mounts[i].read_only = readonly;
            MIMI_LOGI(TAG, "Set mount point '%s' to %s",
                      virt_path, readonly ? "read-only" : "read-write");
            return MIMI_OK;
        }
    }

    /* Check if it's the current workspace */
    if (s_current_workspace >= 0) {
        workspace_t *ws = &s_workspaces[s_current_workspace];
        if (strcmp("/", virt_path) == 0 || strcmp(ws->name, virt_path) == 0) {
            ws->read_only = readonly;
            MIMI_LOGI(TAG, "Set workspace '%s' to %s",
                      ws->name, readonly ? "read-only" : "read-write");
            return MIMI_OK;
        }
    }

    return MIMI_ERR_NOT_FOUND;
}

bool mimi_fs_is_readonly(const char *virt_path)
{
    if (!virt_path) return false;

    /* Check mount points */
    for (int i = 0; i < s_mount_count; i++) {
        if (strcmp(s_mounts[i].virt_path, virt_path) == 0) {
            return s_mounts[i].read_only;
        }
    }

    /* Check current workspace */
    if (s_current_workspace >= 0) {
        workspace_t *ws = &s_workspaces[s_current_workspace];
        if (strcmp("/", virt_path) == 0 || strcmp(ws->name, virt_path) == 0) {
            return ws->read_only;
        }
    }

    return false;
}

/* ==========================================================================
 * File operations
 * ========================================================================== */

mimi_err_t mimi_fs_open(const char *path, const char *mode, mimi_file_t **out_file)
{
    if (!path || !mode || !out_file) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (s_current_workspace < 0) {
        return MIMI_ERR_INVALID_STATE;
    }

    workspace_t *ws = &s_workspaces[s_current_workspace];
    const fs_operations_t *ops = get_impl_ops(ws->type);
    if (!ops) {
        return MIMI_ERR_NOT_FOUND;
    }

    /* Permission check */
    bool write = (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL);
    if (!check_permission(path, write)) {
        return MIMI_ERR_PERMISSION_DENIED;
    }

    /* Path resolution */
    char real_path[1024];
    mimi_err_t err = resolve_path(path, real_path, sizeof(real_path));
    if (err != MIMI_OK) {
        return err;
    }

    MIMI_LOGD(TAG, "open: %s -> %s (mode=%s)", path, real_path, mode);

    /* Call underlying implementation */
    void *impl_handle = NULL;
    err = ops->open(ws, real_path, mode, &impl_handle);
    if (err != MIMI_OK) {
        return err;
    }

    mimi_file_t *file = (mimi_file_t *)calloc(1, sizeof(mimi_file_t));
    if (!file) {
        if (ops->close) ops->close(impl_handle);
        return MIMI_ERR_NO_MEM;
    }

    file->workspace = ws;
    file->impl_handle = impl_handle;
    *out_file = file;

    return MIMI_OK;
}

mimi_err_t mimi_fs_read(mimi_file_t *file, void *buf, size_t max_len, size_t *out_len)
{
    if (!file || !file->workspace) return MIMI_ERR_INVALID_ARG;

    const fs_operations_t *ops = get_impl_ops(file->workspace->type);
    if (!ops || !ops->read) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    return ops->read(file->impl_handle, buf, max_len, out_len);
}

mimi_err_t mimi_fs_write(mimi_file_t *file, const void *buf, size_t len, size_t *out_written)
{
    if (!file || !file->workspace) return MIMI_ERR_INVALID_ARG;

    const fs_operations_t *ops = get_impl_ops(file->workspace->type);
    if (!ops || !ops->write) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    return ops->write(file->impl_handle, buf, len, out_written);
}

mimi_err_t mimi_fs_close(mimi_file_t *file)
{
    if (!file || !file->workspace) return MIMI_ERR_INVALID_ARG;

    const fs_operations_t *ops = get_impl_ops(file->workspace->type);
    mimi_err_t err = MIMI_OK;

    if (ops && ops->close) {
        err = ops->close(file->impl_handle);
    }

    free(file);
    return err;
}

mimi_err_t mimi_fs_seek(mimi_file_t *file, long offset, int whence)
{
    if (!file || !file->workspace) return MIMI_ERR_INVALID_ARG;

    const fs_operations_t *ops = get_impl_ops(file->workspace->type);
    if (!ops || !ops->seek) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    return ops->seek(file->impl_handle, offset, whence);
}

mimi_err_t mimi_fs_tell(mimi_file_t *file, long *out_pos)
{
    if (!file || !file->workspace) return MIMI_ERR_INVALID_ARG;

    const fs_operations_t *ops = get_impl_ops(file->workspace->type);
    if (!ops || !ops->tell) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    return ops->tell(file->impl_handle, out_pos);
}

mimi_err_t mimi_fs_read_line(mimi_file_t *file, char *buf, size_t buf_size, bool *out_eof)
{
    if (!file || !file->workspace) return MIMI_ERR_INVALID_ARG;

    const fs_operations_t *ops = get_impl_ops(file->workspace->type);
    if (!ops || !ops->read_line) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    return ops->read_line(file->impl_handle, buf, buf_size, out_eof);
}

mimi_err_t mimi_fs_remove(const char *path)
{
    if (!path) return MIMI_ERR_INVALID_ARG;

    if (s_current_workspace < 0) {
        return MIMI_ERR_INVALID_STATE;
    }

    workspace_t *ws = &s_workspaces[s_current_workspace];
    const fs_operations_t *ops = get_impl_ops(ws->type);
    if (!ops || !ops->remove) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Permission check */
    if (!check_permission(path, true)) {
        return MIMI_ERR_PERMISSION_DENIED;
    }

    /* Path resolution */
    char real_path[1024];
    mimi_err_t err = resolve_path(path, real_path, sizeof(real_path));
    if (err != MIMI_OK) {
        return err;
    }

    return ops->remove(ws, real_path);
}

mimi_err_t mimi_fs_mkdir_p(const char *dir)
{
    if (!dir) return MIMI_ERR_INVALID_ARG;

    if (s_current_workspace < 0) {
        return MIMI_ERR_INVALID_STATE;
    }

    workspace_t *ws = &s_workspaces[s_current_workspace];
    const fs_operations_t *ops = get_impl_ops(ws->type);
    if (!ops || !ops->mkdir_p) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Permission check */
    if (!check_permission(dir, true)) {
        return MIMI_ERR_PERMISSION_DENIED;
    }

    /* Path resolution */
    char real_path[1024];
    mimi_err_t err = resolve_path(dir, real_path, sizeof(real_path));
    if (err != MIMI_OK) {
        return err;
    }

    return ops->mkdir_p(ws, real_path);
}

mimi_err_t mimi_fs_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return MIMI_ERR_INVALID_ARG;

    if (s_current_workspace < 0) {
        return MIMI_ERR_INVALID_STATE;
    }

    workspace_t *ws = &s_workspaces[s_current_workspace];
    const fs_operations_t *ops = get_impl_ops(ws->type);
    if (!ops || !ops->rename) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Permission check */
    if (!check_permission(old_path, true) || !check_permission(new_path, true)) {
        return MIMI_ERR_PERMISSION_DENIED;
    }

    /* Path resolution */
    char real_old[1024];
    char real_new[1024];
    mimi_err_t err = resolve_path(old_path, real_old, sizeof(real_old));
    if (err != MIMI_OK) {
        return err;
    }
    err = resolve_path(new_path, real_new, sizeof(real_new));
    if (err != MIMI_OK) {
        return err;
    }

    return ops->rename(ws, real_old, real_new);
}

mimi_err_t mimi_fs_exists(const char *path, bool *out_exists)
{
    if (!path || !out_exists) return MIMI_ERR_INVALID_ARG;

    if (s_current_workspace < 0) {
        return MIMI_ERR_INVALID_STATE;
    }

    workspace_t *ws = &s_workspaces[s_current_workspace];
    const fs_operations_t *ops = get_impl_ops(ws->type);
    if (!ops || !ops->exists) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Path resolution */
    char real_path[1024];
    mimi_err_t err = resolve_path(path, real_path, sizeof(real_path));
    if (err != MIMI_OK) {
        return err;
    }

    return ops->exists(ws, real_path, out_exists);
}

/* ==========================================================================
 * Directory operations
 * ========================================================================== */

mimi_err_t mimi_fs_opendir(const char *path, mimi_dir_t **out_dir)
{
    if (!path || !out_dir) return MIMI_ERR_INVALID_ARG;

    if (s_current_workspace < 0) {
        return MIMI_ERR_INVALID_STATE;
    }

    workspace_t *ws = &s_workspaces[s_current_workspace];
    const fs_operations_t *ops = get_impl_ops(ws->type);
    if (!ops || !ops->opendir) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    /* Path resolution */
    char real_path[1024];
    mimi_err_t err = resolve_path(path, real_path, sizeof(real_path));
    if (err != MIMI_OK) {
        return err;
    }

    /* Call underlying implementation */
    void *impl_handle = NULL;
    err = ops->opendir(ws, real_path, &impl_handle);
    if (err != MIMI_OK) {
        return err;
    }

    mimi_dir_t *dir = (mimi_dir_t *)calloc(1, sizeof(mimi_dir_t));
    if (!dir) {
        if (ops->closedir) ops->closedir(impl_handle);
        return MIMI_ERR_NO_MEM;
    }

    dir->workspace = ws;
    dir->impl_handle = impl_handle;
    *out_dir = dir;

    return MIMI_OK;
}

mimi_err_t mimi_fs_readdir(mimi_dir_t *dir, char *name_out, size_t name_out_len, bool *out_has_entry)
{
    if (!dir || !dir->workspace) return MIMI_ERR_INVALID_ARG;

    const fs_operations_t *ops = get_impl_ops(dir->workspace->type);
    if (!ops || !ops->readdir) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    return ops->readdir(dir->impl_handle, name_out, name_out_len, out_has_entry);
}

mimi_err_t mimi_fs_closedir(mimi_dir_t *dir)
{
    if (!dir || !dir->workspace) return MIMI_ERR_INVALID_ARG;

    const fs_operations_t *ops = get_impl_ops(dir->workspace->type);
    mimi_err_t err = MIMI_OK;

    if (ops && ops->closedir) {
        err = ops->closedir(dir->impl_handle);
    }

    free(dir);
    return err;
}

/* ==========================================================================
 * Initialization and shutdown
 * ========================================================================== */

mimi_err_t mimi_fs_init(void)
{
    s_impl_count = 0;
    s_workspace_count = 0;
    s_current_workspace = -1;
    s_mount_count = 0;

    MIMI_LOGI(TAG, "VFS layer initialized");
    return MIMI_OK;
}

mimi_err_t mimi_fs_shutdown(void)
{
    s_impl_count = 0;
    s_workspace_count = 0;
    s_current_workspace = -1;
    s_mount_count = 0;

    MIMI_LOGI(TAG, "VFS layer shutdown");
    return MIMI_OK;
}

/* ==========================================================================
 * Direct POSIX API Functions
 * ========================================================================== */

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <unistd.h>
#endif

bool mimi_fs_exists_direct(const char *path)
{
    if (!path || path[0] == '\0') return false;
    return (access(path, F_OK) == 0);
}

int mimi_fs_mkdir_p_direct(const char *path)
{
    if (!path || path[0] == '\0') return -1;
    
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[len - 1] = '\0';
    }
    
    /* On Windows, skip drive letter (e.g., "C:\") */
    char *start = tmp;
    #ifdef _WIN32
    if (tmp[0] != '\0' && tmp[1] == ':') {
        start = tmp + 2;  /* Skip "C:" */
    }
    #endif
    
    /* Create parent directories */
    for (char *p = start + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            #ifdef _WIN32
            _mkdir(tmp);
            #else
            mkdir(tmp, 0755);
            #endif
            *p = sep;
        }
    }
    
    #ifdef _WIN32
    return _mkdir(tmp);
    #else
    return mkdir(tmp, 0755);
    #endif
}
