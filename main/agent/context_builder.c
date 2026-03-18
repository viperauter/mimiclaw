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

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(path, "r", &f);
    if (err != MIMI_OK) return offset;

    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t n = 0;
    err = mimi_fs_read(f, buf + offset, size - offset - 1, &n);
    offset += (err == MIMI_OK) ? n : 0;
    buf[offset] = '\0';
    mimi_fs_close(f);
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

    /* Agent guidelines (workspace root, created by bootstrap; ignore if missing). */
    off = append_file(buf, size, off, MIMI_DEFAULT_AGENTS_FILE, "Agent Guidelines");

    /* Short memory / skills pointers (details live in policies and files). */
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

    /* Bootstrap files */
    off = append_file(buf, size, off, soul_file, "Personality");
    off = append_file(buf, size, off, user_file, "User Info");

    /* Long-term memory */
    char mem_buf[4096];
    if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == MIMI_OK && mem_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
    }

    /* Recent daily notes (last 3 days) */
    char recent_buf[4096];
    if (memory_read_recent(recent_buf, sizeof(recent_buf), 3) == MIMI_OK && recent_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
    }

    /* Skills */
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
