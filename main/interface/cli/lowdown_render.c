/**
 * Lowdown Markdown Terminal Renderer Implementation
 */

#include "lowdown_render.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#if defined(MIMICLAW_ENABLE_LOWDOWN)
#include <sys/queue.h>
#include "lowdown/lowdown.h"
#endif

#define TAG "lowdown_render"

#define DEFAULT_TERMINAL_WIDTH 80
#define MAX_OUTPUT_SIZE 65536

#if defined(MIMICLAW_ENABLE_LOWDOWN)

static struct {
    bool initialized;
    lowdown_config_t config;
    struct lowdown_opts opts;
} s_lowdown = {0};

static const char *MARKDOWN_INDICATORS[] = {
    "#",
    "##",
    "###",
    "```",
    "```",
    "- ",
    "* ",
    "+ ",
    "1. ",
    "> ",
    "|",
    "---",
    "***",
    "___",
    "`",
    "_",
    "*",
    "[",
    "![",
    NULL
};

bool lowdown_is_markdown(const char *text)
{
    if (!text || !*text) {
        return false;
    }

    const char *trimmed = text;
    while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\n') {
        trimmed++;
    }

    if (!*trimmed) {
        return false;
    }

    size_t line_len = strcspn(trimmed, "\n");
    char first_chars[16] = {0};
    if (line_len < sizeof(first_chars) - 1) {
        strncpy(first_chars, trimmed, line_len);
    } else {
        strncpy(first_chars, trimmed, sizeof(first_chars) - 1);
    }

    if (strstr(first_chars, "```") || strstr(first_chars, "---") || 
        strstr(first_chars, "***") || strstr(first_chars, "___") ||
        strstr(first_chars, "|")) {
        return true;
    }

    for (int i = 0; MARKDOWN_INDICATORS[i] != NULL; i++) {
        if (strncmp(trimmed, MARKDOWN_INDICATORS[i], strlen(MARKDOWN_INDICATORS[i])) == 0) {
            return true;
        }
    }

    return false;
}

int lowdown_render_init(const lowdown_config_t *config)
{
    if (s_lowdown.initialized) {
        MIMI_LOGW(TAG, "Already initialized");
        return 0;
    }

    lowdown_config_t default_cfg = {
        .enabled = true,
        .terminal_width = DEFAULT_TERMINAL_WIDTH,
        .use_ansi = true,
        .escape_special = false,
        .features = LOWDOWN_TABLES | LOWDOWN_FENCED | LOWDOWN_FOOTNOTES |
                    LOWDOWN_AUTOLINK | LOWDOWN_STRIKE | LOWDOWN_HILITE |
                    LOWDOWN_SUPER | LOWDOWN_TASKLIST,
        .output_flags = 0
    };

    if (config) {
        s_lowdown.config = *config;
        /* Treat 0 as "use defaults" so callers don't need to import lowdown flags. */
        if (s_lowdown.config.features == 0) {
            s_lowdown.config.features = default_cfg.features;
        }
    } else {
        s_lowdown.config = default_cfg;
    }

    if (!s_lowdown.config.enabled) {
        MIMI_LOGI(TAG, "Lowdown rendering disabled by config");
        return 0;
    }

    memset(&s_lowdown.opts, 0, sizeof(s_lowdown.opts));
    s_lowdown.opts.type = LOWDOWN_TERM;
    s_lowdown.opts.term.cols = s_lowdown.config.terminal_width > 0 ? 
                                s_lowdown.config.terminal_width : DEFAULT_TERMINAL_WIDTH;
    s_lowdown.opts.term.width = s_lowdown.opts.term.cols;
    s_lowdown.opts.term.hmargin = 0;
    s_lowdown.opts.term.vmargin = 0;
    s_lowdown.opts.term.centre = 0;
    s_lowdown.opts.maxdepth = 16;
    s_lowdown.opts.feat = s_lowdown.config.features;
    s_lowdown.opts.oflags = s_lowdown.config.output_flags;

    if (!s_lowdown.config.use_ansi) {
        s_lowdown.opts.oflags |= LOWDOWN_TERM_NOANSI;
    }

    s_lowdown.initialized = true;
    MIMI_LOGI(TAG, "Initialized (width=%zu, ansi=%d)", 
              s_lowdown.opts.term.cols, s_lowdown.config.use_ansi);
    return 0;
}

int lowdown_render(const char *md_input, char *output, size_t output_size)
{
    if (!s_lowdown.initialized) {
        return -1;
    }

    if (!md_input || !output || output_size == 0) {
        return -1;
    }

    if (output_size > MAX_OUTPUT_SIZE) {
        output_size = MAX_OUTPUT_SIZE;
    }

    char *rendered = NULL;
    size_t rendered_sz = 0;

    if (!lowdown_buf(&s_lowdown.opts, md_input, strlen(md_input),
            &rendered, &rendered_sz, NULL)) {
        return -1;
    }

    size_t copy_len = rendered_sz < (output_size - 1) ? rendered_sz : (output_size - 1);
    memcpy(output, rendered, copy_len);
    output[copy_len] = '\0';
    free(rendered);
    return (int)copy_len;
}

void lowdown_render_free(void)
{
    if (!s_lowdown.initialized) {
        return;
    }

    s_lowdown.initialized = false;
    MIMI_LOGI(TAG, "Freed");
}

void lowdown_render_set_config(const lowdown_config_t *config)
{
    if (!config) {
        return;
    }

    bool was_enabled = s_lowdown.config.enabled;
    s_lowdown.config = *config;

    if (!was_enabled && config->enabled) {
        lowdown_render_init(config);
    } else if (was_enabled && !config->enabled) {
        lowdown_render_free();
    } else if (s_lowdown.initialized && config->enabled) {
        lowdown_render_free();
        lowdown_render_init(config);
    }
}

void lowdown_render_get_config(lowdown_config_t *config)
{
    if (!config) {
        return;
    }
    *config = s_lowdown.config;
}

#else

/* When lowdown is disabled, the header provides no-op macros. */

#endif
