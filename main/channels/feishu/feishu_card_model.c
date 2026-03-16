#include "channels/feishu/feishu_card_model.h"

#include <string.h>

static cJSON *mk_plain_text(const char *content)
{
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "tag", "plain_text");
    cJSON_AddStringToObject(t, "content", content ? content : "");
    return t;
}

static cJSON *mk_markdown_div(const char *md)
{
    cJSON *div = cJSON_CreateObject();
    cJSON_AddStringToObject(div, "tag", "div");
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "tag", "lark_md");
    cJSON_AddStringToObject(text, "content", md ? md : "");
    cJSON_AddItemToObject(div, "text", text);
    return div;
}

static cJSON *mk_button(const feishu_card_button_t *b)
{
    cJSON *btn = cJSON_CreateObject();
    cJSON_AddStringToObject(btn, "tag", "button");
    cJSON_AddItemToObject(btn, "text", mk_plain_text(b && b->text ? b->text : ""));
    if (b && b->type && b->type[0]) {
        cJSON_AddStringToObject(btn, "type", b->type);
    }
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "action", (b && b->value_action) ? b->value_action : "");
    cJSON_AddStringToObject(value, "request_id", (b && b->value_request_id) ? b->value_request_id : "");
    if (b && b->value_target && b->value_target[0]) {
        cJSON_AddStringToObject(value, "target", b->value_target);
    }
    cJSON_AddItemToObject(btn, "value", value);
    return btn;
}

cJSON *feishu_card_render(const feishu_card_model_t *model)
{
    if (!model) return NULL;

    cJSON *card = cJSON_CreateObject();
    if (!card) return NULL;

    cJSON *config = cJSON_CreateObject();
    cJSON_AddBoolToObject(config, "wide_screen_mode", model->wide_screen_mode);
    if (model->update_multi) {
        cJSON_AddBoolToObject(config, "update_multi", true);
    }
    cJSON_AddItemToObject(card, "config", config);

    if ((model->title && model->title[0]) || (model->subtitle && model->subtitle[0])) {
        cJSON *header = cJSON_CreateObject();
        if (model->title && model->title[0]) {
            cJSON_AddItemToObject(header, "title", mk_plain_text(model->title));
        }
        if (model->subtitle && model->subtitle[0]) {
            cJSON_AddItemToObject(header, "subtitle", mk_plain_text(model->subtitle));
        }
        cJSON_AddItemToObject(card, "header", header);
    }

    cJSON *elements = cJSON_CreateArray();
    if (!elements) {
        cJSON_Delete(card);
        return NULL;
    }

    for (size_t i = 0; i < model->block_count; i++) {
        const feishu_card_block_t *blk = &model->blocks[i];
        if (blk->type == FEISHU_CARD_BLOCK_MARKDOWN) {
            cJSON_AddItemToArray(elements, mk_markdown_div(blk->markdown.md));
        } else if (blk->type == FEISHU_CARD_BLOCK_ACTIONS) {
            cJSON *action = cJSON_CreateObject();
            cJSON_AddStringToObject(action, "tag", "action");
            cJSON *actions = cJSON_CreateArray();
            if (blk->actions.buttons && blk->actions.button_count > 0) {
                for (size_t j = 0; j < blk->actions.button_count; j++) {
                    cJSON_AddItemToArray(actions, mk_button(&blk->actions.buttons[j]));
                }
            }
            cJSON_AddItemToObject(action, "actions", actions);
            cJSON_AddItemToArray(elements, action);
        }
    }

    cJSON_AddItemToObject(card, "elements", elements);
    return card;
}

