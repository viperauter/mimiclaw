/**
 * CLI Channel Implementation
 *
 * Adapts to existing CLI terminal to the Channel interface.
 * Routes messages through the shared Command System.
 */

#include "channels/cli/cli_channel.h"
#include "channels/channel_manager.h"
#include "commands/command.h"
#include "cli/cli_terminal.h"
#include "channels/cli/terminal_stdio.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "cli_ch";

/* CLI Channel private data */
typedef struct {
    app_terminal_t *terminal;
    bool initialized;
    bool started;
    bool stdio_started;
} cli_channel_priv_t;

static cli_channel_priv_t s_priv = {0};

/**
 * Initialize CLI Channel
 */
mimi_err_t cli_channel_init_impl(channel_t *ch, const channel_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "CLI Channel already initialized");
        return MIMI_OK;
    }

    /* Initialize CLI terminal framework */
    mimi_err_t err = app_terminal_init();
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to init terminal framework: %s", mimi_err_to_name(err));
        return err;
    }

    /* Store channel reference in private data */
    ch->priv_data = &s_priv;
    s_priv.initialized = true;

    MIMI_LOGI(TAG, "CLI Channel initialized");
    return MIMI_OK;
}

/**
 * Start CLI Channel
 */
mimi_err_t cli_channel_start_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        MIMI_LOGE(TAG, "CLI Channel not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        MIMI_LOGW(TAG, "CLI Channel already started");
        return MIMI_OK;
    }

    /* Start STDIO terminal */
    if (!s_priv.stdio_started) {
        mimi_err_t err = stdio_cli_start();
        if (err != MIMI_OK) {
            MIMI_LOGW(TAG, "stdio_cli_start failed: %s", mimi_err_to_name(err));
            return err;
        }
        s_priv.stdio_started = true;
    }

    /* Note: The actual terminal polling is done in app_terminal_poll_all()
     * which should be called from the main loop */

    s_priv.started = true;
    MIMI_LOGI(TAG, "CLI Channel started");
    return MIMI_OK;
}

/**
 * Stop CLI Channel
 */
mimi_err_t cli_channel_stop_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return MIMI_OK;
    }

    s_priv.started = false;
    MIMI_LOGI(TAG, "CLI Channel stopped");
    return MIMI_OK;
}

/**
 * Destroy CLI Channel
 */
void cli_channel_destroy_impl(channel_t *ch)
{
    (void)ch;

    if (!s_priv.initialized) {
        return;
    }

    if (s_priv.terminal) {
        app_terminal_destroy(s_priv.terminal);
        s_priv.terminal = NULL;
    }

    s_priv.initialized = false;
    s_priv.started = false;

    MIMI_LOGI(TAG, "CLI Channel destroyed");
}

/**
 * Send message through CLI Channel
 */
mimi_err_t cli_channel_send_impl(channel_t *ch, const char *session_id, const char *content)
{
    (void)ch;
    (void)session_id;

    if (!s_priv.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    /* Output to terminal */
    if (s_priv.terminal) {
        app_terminal_output_ln(s_priv.terminal, content);
    } else {
        /* Fallback: direct printf */
        printf("%s\n", content);
    }

    return MIMI_OK;
}

/**
 * Check if CLI Channel is running
 */
bool cli_channel_is_running_impl(channel_t *ch)
{
    (void)ch;
    return s_priv.initialized && s_priv.started;
}

/**
 * Set message callback (not used for CLI, handled via terminal)
 */
void cli_channel_set_on_message(channel_t *ch,
                                 void (*cb)(channel_t *, const char *session_id, const char *content, void *user_data),
                                 void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
    /* CLI uses terminal polling, not callbacks */
}

/**
 * Set connect callback (not used for CLI)
 */
void cli_channel_set_on_connect(channel_t *ch,
                                 void (*cb)(channel_t *, const char *session_id, void *user_data),
                                 void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Set disconnect callback (not used for CLI)
 */
void cli_channel_set_on_disconnect(channel_t *ch,
                                    void (*cb)(channel_t *, const char *session_id, void *user_data),
                                    void *user_data)
{
    (void)ch;
    (void)cb;
    (void)user_data;
}

/**
 * Initialize CLI Channel module
 * Called before registering with Channel Manager
 */
mimi_err_t cli_channel_init(void)
{
    /* Initialize CLI terminal framework */
    mimi_err_t err = app_terminal_init();
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to init terminal framework: %s", mimi_err_to_name(err));
        return err;
    }

    MIMI_LOGI(TAG, "CLI Channel module initialized");
    return MIMI_OK;
}

/**
 * Poll CLI Channel for input
 * This should be called from the main loop
 */
void cli_channel_poll(void)
{
    if (!s_priv.initialized || !s_priv.started) {
        return;
    }

    /* Poll all terminals */
    app_terminal_poll_all();
}

/* CLI Channel instance */
channel_t g_cli_channel = {
    .name = "cli",
    .description = "Command Line Interface Channel",
    .require_auth = false,
    .max_sessions = 1,
    .init = cli_channel_init_impl,
    .start = cli_channel_start_impl,
    .stop = cli_channel_stop_impl,
    .destroy = cli_channel_destroy_impl,
    .send = cli_channel_send_impl,
    .is_running = cli_channel_is_running_impl,
    .set_on_message = cli_channel_set_on_message,
    .set_on_connect = cli_channel_set_on_connect,
    .set_on_disconnect = cli_channel_set_on_disconnect,
    .priv_data = NULL,
    .is_initialized = false,
    .is_started = false,
};
