/**
 * WeChat Login Command Implementation
 *
 * Start WeChat QR login process:
 *   /wechat_login
 */

#include "interface/commands/command.h"
#include "log.h"
#include "channels/wechat/wechat_login.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cmd_wechat_login";

/**
 * Execute wechat_login command
 */
static int cmd_wechat_login_execute(const char **args, int arg_count,
                                    const command_context_t *ctx,
                                    char *output, size_t output_len)
{
    (void)args;
    (void)arg_count;
    (void)ctx;

    if (wechat_login_is_logged_in()) {
        snprintf(output, output_len, "Already logged in to WeChat (bot_id: %s)",
                 wechat_login_get_bot_id() ?: "unknown");
        return 0;
    }

    wechat_login_state_t state = wechat_login_get_state();
    if (state == WECHAT_LOGIN_STATE_PENDING || state == WECHAT_LOGIN_STATE_SCANNED) {
        const wechat_login_status_t *status = wechat_login_get_status();
        snprintf(output, output_len, 
                 "Login already in progress (state: %d). QR URL: %s",
                 state, status->qrcode_url);
        return 0;
    }

    mimi_err_t err = wechat_login_start_qr();
    if (err != MIMI_OK) {
        snprintf(output, output_len, "Failed to start WeChat QR login: %d", err);
        return 1;
    }

    const wechat_login_status_t *status = wechat_login_get_status();
    snprintf(output, output_len,
             "WeChat QR login started!\n"
             "QR URL: %s\n"
             "Please open this URL in your browser and scan with WeChat.\n"
             "Waiting for scan...",
             status->qrcode_url);

    return 0;
}

/**
 * WeChat login command definition
 */
static const command_t cmd_wechat_login = {
    .name = "wechat_login",
    .description = "Start WeChat QR login process",
    .usage = "/wechat_login",
    .execute = cmd_wechat_login_execute,
};

/**
 * Check WeChat login status command
 */
static int cmd_wechat_status_execute(const char **args, int arg_count,
                                     const command_context_t *ctx,
                                     char *output, size_t output_len)
{
    (void)args;
    (void)arg_count;
    (void)ctx;

    if (wechat_login_is_logged_in()) {
        snprintf(output, output_len, "WeChat login status: Logged in (bot_id: %s)",
                 wechat_login_get_bot_id() ?: "unknown");
    } else {
        wechat_login_state_t state = wechat_login_get_state();
        const char *state_str = "Unknown";
        switch(state) {
            case WECHAT_LOGIN_STATE_IDLE: state_str = "Idle (not logged in)"; break;
            case WECHAT_LOGIN_STATE_PENDING: state_str = "Pending QR scan"; break;
            case WECHAT_LOGIN_STATE_SCANNED: state_str = "Scanned, waiting confirm"; break;
            case WECHAT_LOGIN_STATE_EXPIRED: state_str = "QR expired"; break;
            case WECHAT_LOGIN_STATE_ERROR: state_str = "Error"; break;
            default: break;
        }
        snprintf(output, output_len, "WeChat login status: %s", state_str);
    }

    return 0;
}

static const command_t cmd_wechat_status = {
    .name = "wechat_status",
    .description = "Check WeChat login status",
    .usage = "/wechat_status",
    .execute = cmd_wechat_status_execute,
};

/**
 * Initialize WeChat login commands
 */
void cmd_wechat_login_init(void)
{
    int ret = command_register(&cmd_wechat_login);
    if (ret != 0) {
        MIMI_LOGE(TAG, "Failed to register wechat_login command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Registered /wechat_login command");
    }

    ret = command_register(&cmd_wechat_status);
    if (ret != 0) {
        MIMI_LOGE(TAG, "Failed to register wechat_status command: %d", ret);
    } else {
        MIMI_LOGD(TAG, "Registered /wechat_status command");
    }
}