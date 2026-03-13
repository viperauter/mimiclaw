#pragma once

/*
 * Runtime configuration (JSON-backed, nanobot-style layout).
 * See config.json.example and CONFIG.md for schema.
 *
 * This project uses a single runtime config module (`config.c/.h`).
 * There is no separate build-time config header (e.g. mimi_config.h).
 */

#include "mimi_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* String capacity for config fields (do not exceed in JSON) */
#define MIMI_CONFIG_API_KEY_LEN     320
#define MIMI_CONFIG_MODEL_LEN       64
#define MIMI_CONFIG_PROVIDER_LEN    32
#define MIMI_CONFIG_URL_LEN         256
#define MIMI_CONFIG_TOKEN_LEN       128
#define MIMI_CONFIG_HOST_LEN        128
#define MIMI_CONFIG_PORT_LEN        16
#define MIMI_CONFIG_BASE_PATH_LEN   256
#define MIMI_CONFIG_API_PROTOCOL_LEN 16
#define MIMI_CONFIG_ALLOW_FROM_LEN 256

typedef struct mimi_config {
    /* agents.defaults (nanobot-style) */
    char workspace[MIMI_CONFIG_BASE_PATH_LEN]; /* base dir for VFS mapping */
    char timezone[64];                         /* POSIX TZ string */
    int  max_tokens;                           /* LLM max tokens */
    double temperature;                        /* LLM temperature */
    int  max_tool_iterations;                  /* tool loop cap */
    int  memory_window;                        /* max history messages */
    char api_url[MIMI_CONFIG_URL_LEN];         /* optional LLM endpoint override */
    bool send_working_status;                  /* send \"mimi is working...\" */

    /* providers.* (LLM) - resolved provider config */
    char api_key[MIMI_CONFIG_API_KEY_LEN];
    char provider[MIMI_CONFIG_PROVIDER_LEN];
    char model[MIMI_CONFIG_MODEL_LEN];
    char api_base[MIMI_CONFIG_URL_LEN];                /* provider apiBase override */
    char api_protocol[MIMI_CONFIG_API_PROTOCOL_LEN];   /* "openai" | "anthropic" | "" (auto) */

    /* channels.telegram */
    bool telegram_enabled;
    char telegram_token[MIMI_CONFIG_TOKEN_LEN];
    char telegram_allow_from[MIMI_CONFIG_ALLOW_FROM_LEN]; /* comma-separated IDs */

    /* channels.feishu */
    bool feishu_enabled;
    char feishu_app_id[64];
    char feishu_app_secret[128];

    /* channels.qq */
    bool qq_enabled;
    char qq_app_id[64];
    char qq_token[256];

    /* proxy */
    char proxy_host[MIMI_CONFIG_HOST_LEN];
    char proxy_port[MIMI_CONFIG_PORT_LEN];
    char proxy_type[16];

    /* tools.search (Brave Search) */
    char search_api_key[MIMI_CONFIG_TOKEN_LEN];

    /* internal tunables (not nanobot schema; defaults are fine) */
    int bus_queue_len;
    int ws_port;
    int heartbeat_interval_ms;
    int cron_check_interval_ms;

    /* File paths (treated as real filesystem paths; may be absolute or relative) */
    char heartbeat_file[128];
    char cron_file[128];
    char memory_file[128];
    char soul_file[128];
    char user_file[128];
    char skills_prefix[128];
    char session_dir[128];

    /* wifi (for embedded; unused on POSIX) */
    char wifi_ssid[64];
    char wifi_pass[64];
    
    /* network */
    char dns_server[64]; /* DNS server address, e.g., "114.114.114.114" */
    
    /* logging */
    char log_level[16]; /* "error", "warn", "info", "debug" */
} mimi_config_t;

/**
 * Load config from a JSON file (nanobot-style keys).
 * Missing keys are filled from built-in defaults.
 * path == NULL or load failure: use only defaults (no file).
 * Returns MIMI_OK on success or when file is missing (defaults applied).
 */
mimi_err_t mimi_config_load(const char *path);

/**
 * Get the current runtime config (set by mimi_config_load).
 * Never returns NULL; internal static struct is returned.
 */
const mimi_config_t *mimi_config_get(void);

/**
 * Write current config to a JSON file (optional; for persistence).
 */
mimi_err_t mimi_config_save(const char *path);

#ifdef __cplusplus
}
#endif
