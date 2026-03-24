#pragma once

/* Internal config types for config.c only.
 * Public consumers should use config_view.h instead. */

#include <stdbool.h>
#include <stddef.h>

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

typedef struct {
    char name[64];
    char role[64];
    char type[16];
    char system_prompt_file[MIMI_CONFIG_BASE_PATH_LEN];
    char tools[256];
    int  max_iters;
    int  timeout_sec;
} mimi_subagent_config_t;

typedef struct mimi_config {
    char workspace[MIMI_CONFIG_BASE_PATH_LEN];
    char timezone[64];
    int  max_tokens;
    double temperature;
    int  max_tool_iterations;
    int  memory_window;
    char api_url[MIMI_CONFIG_URL_LEN];
    bool send_working_status;

    char api_key[MIMI_CONFIG_API_KEY_LEN];
    char provider[MIMI_CONFIG_PROVIDER_LEN];
    char model[MIMI_CONFIG_MODEL_LEN];
    char api_base[MIMI_CONFIG_URL_LEN];
    char api_protocol[MIMI_CONFIG_API_PROTOCOL_LEN];

    bool telegram_enabled;
    char telegram_token[MIMI_CONFIG_TOKEN_LEN];
    char telegram_allow_from[MIMI_CONFIG_ALLOW_FROM_LEN];

    bool feishu_enabled;
    char feishu_app_id[64];
    char feishu_app_secret[128];

    bool qq_enabled;
    char qq_app_id[64];
    char qq_token[256];

    char proxy_host[MIMI_CONFIG_HOST_LEN];
    char proxy_port[MIMI_CONFIG_PORT_LEN];
    char proxy_type[16];

    char search_api_key[MIMI_CONFIG_TOKEN_LEN];

    int bus_queue_len;
    int ws_port;
    int heartbeat_interval_ms;
    int cron_check_interval_ms;

    char heartbeat_file[128];
    char cron_file[128];
    char memory_file[128];
    char soul_file[128];
    char user_file[128];
    char skills_prefix[128];
    char session_dir[128];

    char dns_server[64];
    char log_level[16];
    bool log_to_file;
    bool log_to_stderr;
    char log_dir[128];
    char log_file[128];
    int  log_max_file_bytes;
    int  log_max_files;

    bool llm_trace_enabled;
    char llm_trace_dir[256];
    int  llm_trace_max_file_bytes;
    int  llm_trace_max_field_bytes;

    mimi_subagent_config_t subagents[8];
    int subagent_count;
} mimi_config_t;

