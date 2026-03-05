#include "workspace_bootstrap.h"

#include "config.h"
#include "fs/fs.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "workspace";

#if defined(__unix__) || defined(__APPLE__)
static int ensure_parent_dir_for_file(const char *file_path)
{
    if (!file_path || file_path[0] == '\0') return -1;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", file_path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;          /* CWD */
    if (slash == tmp) return 0;    /* root */
    *slash = '\0';
    return (mimi_fs_mkdir_p(tmp) == MIMI_OK) ? 0 : -1;
}

static bool write_file_if_missing(const char *path, const char *content)
{
    if (!path || path[0] == '\0') return false;
    bool exists = false;
    if (mimi_fs_exists(path, &exists) == MIMI_OK && exists) return false;

    (void)ensure_parent_dir_for_file(path);
    mimi_file_t *f = NULL;
    if (mimi_fs_open(path, "w", &f) != MIMI_OK) return false;
    if (content && content[0]) {
        size_t written = 0;
        (void)mimi_fs_write(f, content, strlen(content), &written);
    }
    mimi_fs_close(f);
    return true;
}

static void bootstrap_vfs_layout(void)
{
    const mimi_config_t *cfg = mimi_config_get();

    /* Ensure directories for configured files exist (POSIX only). */
    (void)ensure_parent_dir_for_file(cfg->soul_file);
    (void)ensure_parent_dir_for_file(cfg->user_file);
    (void)ensure_parent_dir_for_file(cfg->memory_file);
    (void)ensure_parent_dir_for_file(cfg->heartbeat_file);
    (void)ensure_parent_dir_for_file(cfg->cron_file);

    /* memory daily dir (derived from memory_file) */
    {
        char base[512];
        const char *mem = cfg->memory_file;
        const char *slash = mem ? strrchr(mem, '/') : NULL;
        if (slash) {
            size_t len = (size_t)(slash - mem);
            if (len >= sizeof(base)) len = sizeof(base) - 1;
            memcpy(base, mem, len);
            base[len] = '\0';
        } else {
            snprintf(base, sizeof(base), "memory");
        }
        char daily_dir[520];
        snprintf(daily_dir, sizeof(daily_dir), "%s/daily", base);
        (void)mimi_fs_mkdir_p(daily_dir);
    }

    /* skills and sessions directories */
    if (cfg->skills_prefix[0]) {
        (void)mimi_fs_mkdir_p(cfg->skills_prefix);
    } else {
        (void)mimi_fs_mkdir_p("skills");
    }
    if (cfg->session_dir[0]) {
        (void)mimi_fs_mkdir_p(cfg->session_dir);
    } else {
        (void)mimi_fs_mkdir_p("sessions");
    }

    /* Create bootstrap files if missing (never overwrite). */
    char real[512];

    snprintf(real, sizeof(real), "%s", cfg->soul_file);
    (void)write_file_if_missing(real,
        "# SOUL\n\n"
        "You are MimiClaw, a personal AI assistant.\n"
        "- Be helpful, accurate, and concise.\n"
        "- Use tools when you need up-to-date info or to read/write files.\n"
        "- Persist durable user facts into MEMORY.md.\n");

    snprintf(real, sizeof(real), "%s", cfg->user_file);
    (void)write_file_if_missing(real,
        "# USER\n\n"
        "- Name:\n"
        "- Preferred language:\n"
        "- Location/timezone:\n"
        "- Preferences:\n");

    snprintf(real, sizeof(real), "%s", cfg->memory_file);
    (void)write_file_if_missing(real,
        "# MEMORY\n\n"
        "## Durable facts\n"
        "- \n\n"
        "## Preferences\n"
        "- \n");

    snprintf(real, sizeof(real), "%s", cfg->heartbeat_file);
    (void)write_file_if_missing(real,
        "# HEARTBEAT\n\n"
        "- [ ] Add tasks here. Unchecked items will be picked up automatically.\n");

    snprintf(real, sizeof(real), "%s", cfg->cron_file);
    (void)write_file_if_missing(real, "{\n  \"jobs\": []\n}\n");

    /* Helpful extra docs (not currently consumed by the agent loop).
     * Use relative paths - VFS will resolve them against workspace base. */
    (void)write_file_if_missing("AGENTS.md",
        "# AGENTS\n\n"
        "Guidelines for the assistant.\n"
        "- Prefer clarity over verbosity.\n"
        "- Ask concise questions only when necessary.\n");

    (void)write_file_if_missing("TOOLS.md",
        "# TOOLS\n\n"
        "Tool usage notes.\n"
        "- Use web_search for real-time info.\n"
        "- Use get_current_time for date/time.\n");
}
#endif

mimi_err_t mimi_workspace_bootstrap(const char *config_path,
                                    bool create_starter_config_if_missing)
{
#if defined(__unix__) || defined(__APPLE__)
    const mimi_config_t *cfg = mimi_config_get();

    char base_buf[512] = {0};
    const char *base = NULL;

    if (cfg->workspace[0]) {
        /* Scheme B: keep cfg->workspace as the top-level folder, put VFS under "<workspace>/workspace". */
        size_t n = strlen(cfg->workspace);
        const char *sep = (n > 0 && cfg->workspace[n - 1] == '/') ? "" : "/";
        snprintf(base_buf, sizeof(base_buf), "%s%sworkspace", cfg->workspace, sep);
        base = base_buf;
    } else {
        base = "./spiffs";
    }

    /* Create and activate default workspace */
    mimi_err_t err = mimi_fs_workspace_create("default", base);
    if (err != MIMI_OK && err != MIMI_ERR_FAIL) {
        MIMI_LOGE(TAG, "failed to create workspace: %s", base);
        return MIMI_ERR_FAIL;
    }

    if (mimi_fs_workspace_activate("default") != MIMI_OK) {
        MIMI_LOGE(TAG, "failed to activate workspace: %s", base);
        return MIMI_ERR_FAIL;
    }

    /* Ensure base directory exists */
    (void)mimi_fs_mkdir_p(base);

    bool config_missing = false;
    if (config_path && config_path[0]) {
        bool exists = false;
        (void)mimi_fs_exists(config_path, &exists);
        config_missing = !exists;
    }

    bootstrap_vfs_layout();
    if (config_missing) {
        MIMI_LOGI(TAG, "Bootstrapped workspace layout");
    }

    if (create_starter_config_if_missing && config_missing && config_path && config_path[0]) {
        (void)ensure_parent_dir_for_file(config_path);
        if (mimi_config_save(config_path) == MIMI_OK) {
            MIMI_LOGI(TAG, "Created starter config at %s", config_path);
        } else {
            MIMI_LOGE(TAG, "Failed to create starter config at %s", config_path);
        }
    }

    return MIMI_OK;
#else
    (void)config_path;
    (void)create_starter_config_if_missing;
    return MIMI_OK;
#endif
}

