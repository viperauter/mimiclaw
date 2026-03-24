/**
 * WeChat Login Manager
 *
 * Separate login/state management from the channel implementation.
 * Handles QR code generation, status checking, and token persistence.
 */

#pragma once

#include "mimi_err.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Login states */
typedef enum {
    WECHAT_LOGIN_STATE_IDLE = 0,        /* Not logged in */
    WECHAT_LOGIN_STATE_PENDING,          /* QR code generated, waiting for scan */
    WECHAT_LOGIN_STATE_SCANNED,          /* QR code scanned, waiting for confirm */
    WECHAT_LOGIN_STATE_LOGGED_IN,        /* Successfully logged in */
    WECHAT_LOGIN_STATE_EXPIRED,          /* QR code expired */
    WECHAT_LOGIN_STATE_ERROR,            /* Login error */
} wechat_login_state_t;

/* Login status information */
typedef struct {
    wechat_login_state_t state;
    char qrcode_id[256];                 /* QR code identifier (for status check) */
    char qrcode_url[256];                /* QR code image URL */
    char bot_token[512];                 /* Bot token after login */
    char bot_id[128];                    /* Bot ID after login */
    char user_id[128];                   /* User ID after login */
    time_t qrcode_created_at;            /* QR code creation time */
    char last_error[256];                /* Last error message */
} wechat_login_status_t;

/* Login callback function type */
typedef void (*wechat_login_state_cb)(wechat_login_state_t new_state, 
                                      const wechat_login_status_t *status,
                                      void *user_data);

/**
 * Initialize login manager
 */
mimi_err_t wechat_login_manager_init(void);

/**
 * Cleanup login manager
 */
void wechat_login_manager_cleanup(void);

/**
 * Start QR login flow - returns login URL for scanning
 */
mimi_err_t wechat_login_start_qr(void);

/**
 * Check QR login status (call repeatedly after start_qr)
 */
mimi_err_t wechat_login_check_status(void);

/**
 * Get current login state
 */
wechat_login_state_t wechat_login_get_state(void);

/**
 * Get full login status information
 */
const wechat_login_status_t* wechat_login_get_status(void);

/**
 * Check if logged in
 */
bool wechat_login_is_logged_in(void);

/**
 * Get bot token (if logged in)
 */
const char* wechat_login_get_token(void);

/**
 * Get bot ID (if logged in)
 */
const char* wechat_login_get_bot_id(void);

/**
 * Get user ID (if logged in)
 */
const char* wechat_login_get_user_id(void);

/**
 * Manual login with existing token
 */
mimi_err_t wechat_login_with_token(const char *bot_token, 
                                   const char *bot_id,
                                   const char *user_id);

/**
 * Logout and clear credentials
 */
void wechat_login_logout(void);

/**
 * Set callback for state changes
 */
void wechat_login_set_callback(wechat_login_state_cb cb, void *user_data);

/**
 * Save login credentials to persistent storage (config)
 */
mimi_err_t wechat_login_save_to_config(void);

/**
 * Load login credentials from config
 */
mimi_err_t wechat_login_load_from_config(void);

#ifdef __cplusplus
}
#endif
