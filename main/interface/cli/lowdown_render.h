/**
 * Lowdown Markdown Terminal Renderer
 *
 * Provides markdown rendering for terminal output using the lowdown library.
 * This module can be disabled at compile time using MIMICLAW_ENABLE_LOWDOWN macro.
 */

#ifndef LOWDOWN_RENDER_H
#define LOWDOWN_RENDER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(MIMICLAW_ENABLE_LOWDOWN)

#define LOWDOWN_RENDER_VERSION 1

typedef struct {
    bool enabled;
    int terminal_width;
    bool use_ansi;
    bool escape_special;
    unsigned int features;
    unsigned int output_flags;
} lowdown_config_t;

int lowdown_render_init(const lowdown_config_t *config);

int lowdown_render(const char *md_input, char *output, size_t output_size);

void lowdown_render_free(void);

void lowdown_render_set_config(const lowdown_config_t *config);

void lowdown_render_get_config(lowdown_config_t *config);

bool lowdown_is_markdown(const char *text);

#else

#define lowdown_render_init(config) (-1)
#define lowdown_render(md_input, output, output_size) (-1)
#define lowdown_render_free() do {} while(0)
#define lowdown_render_set_config(config) do {} while(0)
#define lowdown_render_get_config(config) do { if (config) { config->enabled = false; } } while(0)
#define lowdown_is_markdown(text) (false)

#endif

#ifdef __cplusplus
}
#endif

#endif /* LOWDOWN_RENDER_H */
