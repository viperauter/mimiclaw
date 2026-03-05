#pragma once

#include "mimi_err.h"

/**
 * Initialize the Telegram bot.
 */
mimi_err_t telegram_bot_init(void);

/**
 * Start the Telegram polling task (long polling on Core 0).
 */
mimi_err_t telegram_bot_start(void);

/**
 * Send a text message to a Telegram chat.
 * Automatically splits messages longer than 4096 chars.
 * @param chat_id  Telegram chat ID (numeric string)
 * @param text     Message text (supports Markdown)
 */
mimi_err_t telegram_send_message(const char *chat_id, const char *text);

/**
 * Save the Telegram bot token to NVS.
 */
mimi_err_t telegram_set_token(const char *token);

