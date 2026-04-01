#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Virtual File System (VFS) Layer
 *
 * Provides an abstraction over platform-specific file system implementations.
 * Supports multiple workspaces, path mapping, and permission control.
 *
 * Design goals:
 * - Upper layers should not call fopen/fread directly
 * - Support multiple isolated workspaces
 * - Virtual path mapping for flexibility
 * - Permission control (read-only mounts)
 * ------------------------------------------------------------------------- */

/* Opaque file handle */
typedef struct mimi_file mimi_file_t;

/* Opaque directory handle */
typedef struct mimi_dir mimi_dir_t;

/* File system type enumeration */
typedef enum {
    FS_TYPE_POSIX = 0,
    FS_TYPE_SPIFFS,
    FS_TYPE_CUSTOM
} fs_type_t;

/* File system operation function pointers for implementation registration */
typedef struct fs_operations {
    mimi_err_t (*open)(void *ctx, const char *path, const char *mode, void **out_handle);
    mimi_err_t (*read)(void *handle, void *buf, size_t max_len, size_t *out_len);
    mimi_err_t (*write)(void *handle, const void *buf, size_t len, size_t *out_written);
    mimi_err_t (*close)(void *handle);
    mimi_err_t (*seek)(void *handle, long offset, int whence);
    mimi_err_t (*tell)(void *handle, long *out_pos);
    mimi_err_t (*read_line)(void *handle, char *buf, size_t buf_size, bool *out_eof);
    mimi_err_t (*remove)(void *ctx, const char *path);
    mimi_err_t (*mkdir_p)(void *ctx, const char *dir);
    mimi_err_t (*rename)(void *ctx, const char *old_path, const char *new_path);
    mimi_err_t (*opendir)(void *ctx, const char *path, void **out_handle);
    mimi_err_t (*readdir)(void *handle, char *name_out, size_t name_out_len, bool *out_has_entry);
    mimi_err_t (*closedir)(void *handle);
    mimi_err_t (*exists)(void *ctx, const char *path, bool *out_exists);
} fs_operations_t;

/* ==========================================================================
 * VFS Initialization and Implementation Registration
 * ========================================================================== */

/**
 * Initialize the VFS layer.
 * Must be called before any other VFS operations.
 *
 * @return MIMI_OK on success, error code on failure
 */
mimi_err_t mimi_fs_init(void);

/**
 * Shutdown the VFS layer and release all resources.
 */
mimi_err_t mimi_fs_shutdown(void);

/**
 * Register a file system implementation.
 *
 * @param type File system type identifier
 * @param ops  Pointer to operations table
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_register_impl(fs_type_t type, const fs_operations_t *ops);

/* ==========================================================================
 * Workspace Management
 *
 * Workspaces provide isolated file system environments. Each workspace has
 * its own base directory and mount points. Only one workspace is active at
 * a time for the current thread.
 * ========================================================================== */

/**
 * Create a new workspace.
 *
 * @param name      Unique workspace name (e.g., "default", "project_a")
 * @param base_dir  Base directory for this workspace (all relative paths
 *                  are resolved against this directory)
 * @return MIMI_OK on success, MIMI_ERR_EXISTS if name already in use
 */
mimi_err_t mimi_fs_workspace_create(const char *name, const char *base_dir);

/**
 * Delete a workspace.
 * The workspace must not be currently active.
 *
 * @param name Workspace name
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_workspace_delete(const char *name);

/**
 * Activate a workspace for the current thread.
 * All subsequent file operations will use this workspace's context.
 *
 * @param name Workspace name to activate
 * @return MIMI_OK on success, MIMI_ERR_NOT_FOUND if workspace doesn't exist
 */
mimi_err_t mimi_fs_workspace_activate(const char *name);

/**
 * Get the name of the currently active workspace.
 *
 * @param name_out     Buffer to store workspace name
 * @param name_out_len Size of name_out buffer
 * @return MIMI_OK on success, MIMI_ERR_INVALID_STATE if no workspace active
 */
mimi_err_t mimi_fs_workspace_get_current(char *name_out, size_t name_out_len);

/**
 * Set the base directory for the current workspace.
 *
 * @param base_dir New base directory path
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_set_base(const char *base_dir);

/**
 * Resolve a virtual VFS path into a real filesystem path using current workspace.
 *
 * @param virt_path Virtual path (relative or absolute)
 * @param out_real_path Output buffer for resolved real path
 * @param size Output buffer size
 * @return MIMI_OK on success, error code otherwise
 */
mimi_err_t mimi_fs_resolve_path(const char *virt_path, char *out_real_path, size_t size);

/* ==========================================================================
 * Path Mapping (Mount Points)
 *
 * Map virtual paths to real file system paths. Useful for:
 * - Exposing system directories under virtual names
 * - Creating read-only views of directories
 * - Organizing file system layout
 * ========================================================================== */

/**
 * Mount a real directory at a virtual path.
 *
 * Example:
 *   mimi_fs_mount("/etc", "/etc");      // Access /etc as /etc
 *   mimi_fs_mount("/config", "/etc");   // Access /etc as /config
 *
 * @param virt_path Virtual path (must start with '/')
 * @param real_path Real file system path
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_mount(const char *virt_path, const char *real_path);

/**
 * Unmount a virtual path.
 *
 * @param virt_path Virtual path to unmount
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_unmount(const char *virt_path);

/* ==========================================================================
 * Permission Control
 * ========================================================================== */

/**
 * Set read-only status for a mount point.
 *
 * @param virt_path Virtual path of mount point
 * @param readonly  true to make read-only, false for read-write
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_set_readonly(const char *virt_path, bool readonly);

/**
 * Check if a mount point is read-only.
 *
 * @param virt_path Virtual path of mount point
 * @return true if read-only, false otherwise
 */
bool mimi_fs_is_readonly(const char *virt_path);

/* ==========================================================================
 * File Operations
 *
 * All file operations use the currently active workspace.
 * ========================================================================== */

/**
 * Open a file.
 *
 * @param path    File path (relative to workspace base, or absolute virtual path)
 * @param mode    Open mode: "r", "w", "a", "r+", "w+", "a+"
 * @param out_file Output file handle
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_open(const char *path, const char *mode, mimi_file_t **out_file);

/**
 * Read data from a file.
 *
 * @param file   File handle
 * @param buf    Buffer to read into
 * @param max_len Maximum bytes to read
 * @param out_len Actual bytes read (may be NULL)
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_read(mimi_file_t *file, void *buf, size_t max_len, size_t *out_len);

/**
 * Write data to a file.
 *
 * @param file       File handle
 * @param buf        Data to write
 * @param len        Number of bytes to write
 * @param out_written Actual bytes written (may be NULL)
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_write(mimi_file_t *file, const void *buf, size_t len, size_t *out_written);

/**
 * Close a file.
 *
 * @param file File handle
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_close(mimi_file_t *file);

/**
 * Seek to a position in a file.
 *
 * @param file   File handle
 * @param offset Byte offset
 * @param whence SEEK_SET, SEEK_CUR, or SEEK_END
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_seek(mimi_file_t *file, long offset, int whence);

/**
 * Get current file position.
 *
 * @param file   File handle
 * @param out_pos Output position
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_tell(mimi_file_t *file, long *out_pos);

/**
 * Read a line from a file.
 *
 * @param file     File handle
 * @param buf      Buffer to store line
 * @param buf_size Buffer size
 * @param out_eof  Set to true if end of file reached (may be NULL)
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_read_line(mimi_file_t *file, char *buf, size_t buf_size, bool *out_eof);

/**
 * Delete a file.
 *
 * @param path File path
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_remove(const char *path);

/**
 * Create a directory and all parent directories.
 *
 * @param dir Directory path
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_mkdir_p(const char *dir);

/**
 * Rename/move a file or directory.
 *
 * @param old_path Source path
 * @param new_path Destination path
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_rename(const char *old_path, const char *new_path);

/**
 * Check if a file or directory exists.
 *
 * @param path       Path to check
 * @param out_exists Set to true if exists, false otherwise
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_exists(const char *path, bool *out_exists);

/* ==========================================================================
 * Directory Operations
 * ========================================================================== */

/**
 * Open a directory for reading.
 *
 * @param path    Directory path
 * @param out_dir Output directory handle
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_opendir(const char *path, mimi_dir_t **out_dir);

/**
 * Read next directory entry.
 *
 * @param dir          Directory handle
 * @param name_out     Buffer to store entry name
 * @param name_out_len Size of name_out buffer
 * @param out_has_entry Set to false if no more entries (may be NULL)
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_readdir(mimi_dir_t *dir, char *name_out, size_t name_out_len, bool *out_has_entry);

/**
 * Close a directory.
 *
 * @param dir Directory handle
 * @return MIMI_OK on success
 */
mimi_err_t mimi_fs_closedir(mimi_dir_t *dir);

/* ==========================================================================
 * Direct POSIX API Functions
 *
 * These functions bypass the VFS layer and work directly with the file system.
 * Useful for absolute paths outside the workspace.
 * ========================================================================== */

/**
 * Check if a file or directory exists using direct POSIX API.
 * This bypasses the VFS layer and works with absolute paths.
 *
 * @param path Path to check
 * @return true if exists, false otherwise
 */
bool mimi_fs_exists_direct(const char *path);

/**
 * Create a directory and all parent directories using direct POSIX API.
 * This bypasses the VFS layer and works with absolute paths.
 *
 * @param dir Directory path
 * @return 0 on success, -1 on error
 */
int mimi_fs_mkdir_p_direct(const char *dir);

#ifdef __cplusplus
}
#endif
