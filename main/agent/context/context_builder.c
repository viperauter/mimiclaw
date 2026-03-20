#include "context_builder.h"
#include "config.h"
#include "config_view.h"
#include "memory/memory_store.h"
#include "skills/skill_loader.h"

#include <stdio.h>
#include <string.h>
#include "log.h"
#include "fs/fs.h"

static const char *TAG = "context";

/* Check if file content looks like unmodified template content.
 * Returns true if the file contains placeholder patterns that users
 * should customize before the content becomes meaningful.
 */
static bool is_template_content(const char *content, size_t len)
{
    if (!content || len == 0) {
        return true;
    }

    /* Template marker patterns - check for unmodified placeholders */
    const char *patterns[] = {
        "- Name:\n",                    /* USER.md empty name field */
        "- Name: (not set)",           /* Default placeholder value */
        "- Preferred language:\n",     /* Empty language preference */
        "- Location/timezone:\n",      /* Empty timezone field */
        "- Preferences:\n",            /* Empty preferences list */
        "## Durable facts\n- \n",      /* MEMORY.md empty section */
        "- [ ] Add tasks here",        /* HEARTBEAT.md template */
        NULL
    };

    for (int i = 0; patterns[i]; i++) {
        if (strstr(content, patterns[i]) != NULL) {
            return true;
        }
    }

    /* Also consider very short files with default content as templates */
    if (len < 100) {
        const char *default_patterns[] = {
            "# SOUL\n\nYou are MimiClaw",  /* Default SOUL.md header */
            "# USER\n\n",                   /* Empty USER.md header */
            "# MEMORY\n\n",                 /* Empty MEMORY.md header */
            NULL
        };
        for (int i = 0; default_patterns[i]; i++) {
            if (strstr(content, default_patterns[i]) == content) {
                return true;
            }
        }
    }

    return false;
}

/* Append file content to buffer with template detection.
 * Skips files that contain unmodified template content to avoid polluting
 * the system prompt with placeholder values.
 */
static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(path, "r", &f);
    if (err != MIMI_OK) return offset;

    /* Read into temporary buffer for template detection first */
    char temp[4096];
    size_t n = 0;
    err = mimi_fs_read(f, temp, sizeof(temp) - 1, &n);
    temp[n] = '\0';
    mimi_fs_close(f);
    
    if (err != MIMI_OK || n == 0) {
        return offset;
    }

    /* Skip if this is unmodified template content */
    if (is_template_content(temp, n)) {
        MIMI_LOGI(TAG, "Skipping template file: %s (user not customized yet)", path);
        return offset;
    }

    /* Content is user-modified, append to system prompt */
    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t copy_len = (n < size - offset - 1) ? n : (size - offset - 1);
    memcpy(buf + offset, temp, copy_len);
    offset += copy_len;
    buf[offset] = '\0';
    
    return offset;
}

mimi_err_t context_build_system_prompt(char *buf, size_t size)
{
    mimi_cfg_obj_t files = mimi_cfg_section("files");
    const char *memory_file = mimi_cfg_get_str(files, "memoryFile", MIMI_DEFAULT_MEMORY_FILE);
    const char *skills_prefix = mimi_cfg_get_str(files, "skillsPrefix", MIMI_DEFAULT_SKILLS_PREFIX);
    const char *soul_file = mimi_cfg_get_str(files, "soulFile", MIMI_DEFAULT_SOUL_FILE);
    const char *user_file = mimi_cfg_get_str(files, "userFile", MIMI_DEFAULT_USER_FILE);
    size_t off = 0;

    off += snprintf(buf + off, size - off,
        "# MimiClaw\n\n"
        "You are MimiClaw, a personal AI assistant.\n"
        "Be helpful, accurate, and concise.\n");

    off = append_file(buf, size, off, MIMI_DEFAULT_AGENTS_FILE, "Agent Guidelines");

    off += snprintf(buf + off, size - off,
        "\n## Memory\n\n"
        "- Long-term memory: %s\n"
        "- Daily notes: %s/daily/<YYYY-MM-DD>.md\n\n"
        "Use memory to retain durable user facts and preferences.\n\n"
        "## Skills\n\n"
        "Skills live under %s. When relevant, load the skill file before acting.\n",
        memory_file,
        "memory",
        skills_prefix);

    off = append_file(buf, size, off, soul_file, "Personality");
    off = append_file(buf, size, off, user_file, "User Info");

    char mem_buf[4096];
    if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == MIMI_OK && mem_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
    }

    char recent_buf[4096];
    if (memory_read_recent(recent_buf, sizeof(recent_buf), 3) == MIMI_OK && recent_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
    }

    char skills_buf[2048];
    size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
    if (skills_len > 0) {
        off += snprintf(buf + off, size - off,
            "\n## Available Skills\n\n"
            "Available skills (use read_file to load full instructions):\n%s\n",
            skills_buf);
    }

    MIMI_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return MIMI_OK;
}

bool context_needs_first_run_setup(void)
{
    mimi_cfg_obj_t files = mimi_cfg_section("files");
    const char *soul_file = mimi_cfg_get_str(files, "soulFile", MIMI_DEFAULT_SOUL_FILE);
    const char *user_file = mimi_cfg_get_str(files, "userFile", MIMI_DEFAULT_USER_FILE);
    
    const char *check_files[] = { soul_file, user_file, NULL };
    
    for (int i = 0; check_files[i]; i++) {
        mimi_file_t *f = NULL;
        if (mimi_fs_open(check_files[i], "r", &f) == MIMI_OK) {
            char temp[512];
            size_t n = 0;
            if (mimi_fs_read(f, temp, sizeof(temp) - 1, &n) == MIMI_OK && n > 0) {
                temp[n] = '\0';
                if (is_template_content(temp, n)) {
                    mimi_fs_close(f);
                    return true;
                }
            }
            mimi_fs_close(f);
        }
    }
    
    return false;
}

