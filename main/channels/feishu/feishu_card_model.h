#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

/* A minimal, structured card description that can be rendered to Feishu "interactive" card JSON.
 * This keeps UI structure out of business logic while avoiding brittle string templates. */

typedef enum {
    FEISHU_CARD_BLOCK_MARKDOWN = 0,
    FEISHU_CARD_BLOCK_ACTIONS,
} feishu_card_block_type_t;

typedef struct {
    const char *text;              /* button label */
    const char *type;              /* e.g. "primary" or NULL */
    /* value payload (will be encoded into "value") */
    const char *value_action;      /* "ACCEPT"/"REJECT"/... */
    const char *value_request_id;  /* request id */
    const char *value_target;      /* tool name (optional) */
} feishu_card_button_t;

typedef struct {
    feishu_card_block_type_t type;
    union {
        struct {
            const char *md;
        } markdown;
        struct {
            const feishu_card_button_t *buttons;
            size_t button_count;
        } actions;
    };
} feishu_card_block_t;

typedef struct {
    bool wide_screen_mode;
    bool update_multi;
    const char *title;     /* plain text */
    const char *subtitle;  /* plain text */
    const feishu_card_block_t *blocks;
    size_t block_count;
} feishu_card_model_t;

/* Render a model into a cJSON card object (caller owns returned cJSON*). */
cJSON *feishu_card_render(const feishu_card_model_t *model);

