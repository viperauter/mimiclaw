#include "workspace_bootstrap.h"

#include "config.h"
#include "config_view.h"
#include "fs/fs.h"
#include "log.h"
#include "path_utils.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "workspace";

static int ensure_parent_dir_for_file(const char *file_path)
{
    if (!file_path || file_path[0] == '\0') return -1;
    char dir[512];
    if (mimi_path_dirname(file_path, dir, sizeof(dir)) != 0) {
        return 0;  /* No directory component */
    }
    return (mimi_fs_mkdir_p(dir) == MIMI_OK) ? 0 : -1;
}

/* Ensure parent directory exists using direct POSIX API for absolute paths */
static int ensure_parent_dir_posix(const char *file_path)
{
    if (!file_path || file_path[0] == '\0') return -1;
    char dir[512];
    if (mimi_path_dirname(file_path, dir, sizeof(dir)) != 0) {
        return 0;  /* No directory component */
    }
    return mimi_fs_mkdir_p_direct(dir);
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
    mimi_cfg_obj_t files = mimi_cfg_section("files");

    const char *soul_file = mimi_cfg_get_str(files, "soulFile", "config/SOUL.md");
    const char *user_file = mimi_cfg_get_str(files, "userFile", "config/USER.md");
    const char *memory_file = mimi_cfg_get_str(files, "memoryFile", "memory/MEMORY.md");
    const char *heartbeat_file = mimi_cfg_get_str(files, "heartbeatFile", "HEARTBEAT.md");
    const char *cron_file = mimi_cfg_get_str(files, "cronFile", "cron.json");
    const char *skills_prefix = mimi_cfg_get_str(files, "skillsPrefix", "skills/");
    const char *session_dir = mimi_cfg_get_str(files, "sessionDir", "sessions");

    /* Ensure directories for configured files exist (POSIX only). */
    (void)ensure_parent_dir_for_file(soul_file);
    (void)ensure_parent_dir_for_file(user_file);
    (void)ensure_parent_dir_for_file(memory_file);
    (void)ensure_parent_dir_for_file(heartbeat_file);
    (void)ensure_parent_dir_for_file(cron_file);

    /* memory daily dir (derived from memory_file) */
    {
        char base[512];
        if (mimi_path_dirname(memory_file, base, sizeof(base)) != 0) {
            strncpy(base, "memory", sizeof(base) - 1);
            base[sizeof(base) - 1] = '\0';
        }
        char daily_dir[520];
        mimi_path_join(base, "daily", daily_dir, sizeof(daily_dir));
        (void)mimi_fs_mkdir_p(daily_dir);
    }

    /* skills and sessions directories */
    if (skills_prefix && skills_prefix[0]) {
        (void)mimi_fs_mkdir_p(skills_prefix);
    } else {
        (void)mimi_fs_mkdir_p("skills");
    }
    if (session_dir && session_dir[0]) {
        (void)mimi_fs_mkdir_p(session_dir);
    } else {
        (void)mimi_fs_mkdir_p("sessions");
    }

    /* Create bootstrap files if missing (never overwrite). */
    char real[512];

    snprintf(real, sizeof(real), "%s", soul_file);
    (void)write_file_if_missing(real,
        "# SOUL\n\n"
        "You are MimiClaw, a personal AI assistant.\n"
        "- Be helpful, accurate, and concise.\n"
        "- Use tools when you need up-to-date info or to read/write files.\n"
        "- Persist durable user facts into MEMORY.md.\n");

    snprintf(real, sizeof(real), "%s", user_file);
    (void)write_file_if_missing(real,
        "# USER\n\n"
        "- Name:\n"
        "- Preferred language:\n"
        "- Location/timezone:\n"
        "- Preferences:\n");

    snprintf(real, sizeof(real), "%s", memory_file);
    (void)write_file_if_missing(real,
        "# MEMORY\n\n"
        "## Durable facts\n"
        "- \n\n"
        "## Preferences\n"
        "- \n");

    snprintf(real, sizeof(real), "%s", heartbeat_file);
    (void)write_file_if_missing(real,
        "# HEARTBEAT\n\n"
        "- [ ] Add tasks here. Unchecked items will be picked up automatically.\n");

    snprintf(real, sizeof(real), "%s", cron_file);
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

#if MIMI_ENABLE_SUBAGENT
    /* Bootstrap subagent SYSTEM prompts (never overwrite). */
    mimi_cfg_obj_t sub_arr = mimi_cfg_get_arr(mimi_cfg_section("agents"), "subagents");
    int sa_n = mimi_cfg_arr_size(sub_arr);
    for (int i = 0; i < sa_n; i++) {
        mimi_cfg_obj_t sa = mimi_cfg_arr_get(sub_arr, i);
        const char *system_prompt_file = mimi_cfg_get_str(sa, "systemPromptFile", "");
        if (!system_prompt_file || !system_prompt_file[0]) continue;

        char system_path[512];
        if (mimi_path_is_absolute(system_prompt_file)) {
            snprintf(system_path, sizeof(system_path), "%s", system_prompt_file);
        } else {
            /* Treat relative path as relative to workspace base. */
            mimi_path_join(workspace, system_prompt_file, system_path, sizeof(system_path));
        }

        (void)write_file_if_missing(system_path,
            "# SubAgent SYSTEM\n\n"
            "Role: (fill me)\n\n"
            "Guidelines:\n"
            "- Clearly define what this subagent should do.\n"
            "- Keep scope tight; return structured, actionable output.\n");
    }
#endif
}

mimi_err_t mimi_workspace_bootstrap(const char *config_path,
                                    bool create_starter_config_if_missing)
{
    mimi_cfg_obj_t defaults = mimi_cfg_get_obj(mimi_cfg_section("agents"), "defaults");
    const char *workspace = mimi_cfg_get_str(defaults, "workspace", "./");

    const char *base = NULL;

    if (workspace && workspace[0]) {
        /* Use configured workspace directly as the VFS base directory */
        base = workspace;
    } else {
        base = "./";
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

    /* Ensure base directory exists using direct POSIX API for absolute paths */
    if (base[0] == '/' || (base[0] != '\0' && base[1] == ':')) {
        /* Absolute path - use direct POSIX API */
        (void)mimi_fs_mkdir_p_direct(base);
    } else {
        /* Relative path - use VFS */
        (void)mimi_fs_mkdir_p(base);
    }

    /* Check if config exists using direct POSIX API (for absolute paths outside VFS) */
    bool config_missing = false;
    if (config_path && config_path[0]) {
        config_missing = !mimi_fs_exists_direct(config_path);
    }

    /* If config is missing and we are allowed to create one, write it first.
     * This allows bootstrap_vfs_layout() to see agents.subagents and create SYSTEM.md templates. */
    if (create_starter_config_if_missing && config_missing && config_path && config_path[0]) {
        (void)ensure_parent_dir_posix(config_path);
        /* Inject default subagents for out-of-box use (runtime can disable via agents.defaults.subagentsEnabled=false). */
        if (mimi_config_save_starter(config_path, true) == MIMI_OK) {
            MIMI_LOGI(TAG, "Created starter config at %s", config_path);
            /* Reload so config_view reflects the newly created file. */
            (void)mimi_config_load(config_path);
        } else {
            MIMI_LOGE(TAG, "Failed to create starter config at %s", config_path);
        }
    }

    bootstrap_vfs_layout();
    if (config_missing) {
        MIMI_LOGI(TAG, "Bootstrapped workspace layout");
    }

    return MIMI_OK;
}
