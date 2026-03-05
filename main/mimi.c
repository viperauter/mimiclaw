#include "log.h"
#include "fs/fs.h"
#include "kv.h"
#include "mimi_time.h"
#include "os/os.h"
#include "runtime.h"

#include "config.h"
#include "workspace_bootstrap.h"

#include "bus/message_bus.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "skills/skill_loader.h"
#include "tools/tool_registry.h"
#include "agent/agent_loop.h"

#include "llm/llm_proxy.h"
#include "proxy/http_proxy.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "gateway/ws_server.h"
#include "telegram/telegram_bot.h"

#include "cli/stdio_cli_posix.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <getopt.h>

/* Global flag to signal application shutdown */
static volatile bool s_should_exit = false;

void mimi_request_exit(void)
{
    s_should_exit = true;
}

bool mimi_should_exit(void)
{
    return s_should_exit;
}

static void outbound_dispatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != MIMI_OK) continue;
        if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            ws_server_send(msg.chat_id, msg.content ? msg.content : "");
        } else if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            telegram_send_message(msg.chat_id, msg.content ? msg.content : "");
        } else if (strcmp(msg.channel, MIMI_CHAN_CLI) == 0) {
            mimi_tty_printf("[cli:%s] %s\n", msg.chat_id, msg.content ? msg.content : "");
        } else {
            MIMI_LOGW("dispatch", "Unhandled channel %s for chat %s", msg.channel, msg.chat_id);
        }
        free(msg.content);
    }
}

static void parse_args(int argc, char **argv, bool *enable_logs, const char **config_path, const char **log_level)
{
    *enable_logs = false;
    *config_path = NULL;
    *log_level = NULL;
    
    static struct option long_options[] = {
        {"logs", optional_argument, 0, 'l'},
        {"config", required_argument, 0, 'c'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "c:l::h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'c':
                *config_path = optarg;
                break;
            case 'l':
                *enable_logs = true;
                if (optarg) {
                    *log_level = optarg;
                }
                break;
            case 'h':
                printf("Usage: mimiclaw [OPTIONS]\n");
                printf("Options:\n");
                printf("  -c, --config <path>    Specify config file path\n");
                printf("  -l, --logs [level]     Enable logging (level: error, warn, info, debug)\n");
                printf("  -h, --help             Show this help message\n");
                exit(0);
                break;
            case '?':
                break;
            default:
                break;
        }
    }
    
    // Handle positional argument for config path (only if no config option was provided)
    if (optind < argc && *config_path == NULL) {
        // Check if this is not an option (doesn't start with '-')
        if (argv[optind][0] != '-') {
            // Special case: if we have --logs without argument, the next arg might be log level
            if (*enable_logs && *log_level == NULL) {
                *log_level = argv[optind];
            } else {
                *config_path = argv[optind];
            }
        }
    }
}

int main(int argc, char **argv)
{
    bool enable_logs = false;
    const char *config_path = NULL;
    const char *log_level = NULL;
    
    MIMI_LOGI("main", "MimiClaw starting");

    parse_args(argc, argv, &enable_logs, &config_path, &log_level);
    
    char config_buf[512] = {0};
    if (!config_path) {
        const char *home = getenv("HOME");
        if (home && home[0] != '\0') {
            snprintf(config_buf, sizeof(config_buf), "%s/.mimiclaw/config.json", home);
            config_path = config_buf;
        } else {
            config_path = "./config.json";
        }
    }

    /* Initialize VFS layer first */
    if (mimi_fs_init() != MIMI_OK) {
        MIMI_LOGE("main", "VFS initialization failed");
        return 1;
    }
    
    /* Register POSIX file system implementation */
    extern void posix_fs_register(void);
    posix_fs_register();
    
    /* Create and activate default workspace for config loading */
    char default_workspace[512];
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        snprintf(default_workspace, sizeof(default_workspace), "%s/.mimiclaw/workspace", home);
    } else {
        snprintf(default_workspace, sizeof(default_workspace), "./spiffs");
    }
    mimi_err_t err = mimi_fs_workspace_create("default", default_workspace);
    if (err != MIMI_OK && err != MIMI_ERR_FAIL) {
        MIMI_LOGE("main", "failed to create default workspace");
        return 1;
    }
    if (mimi_fs_workspace_activate("default") != MIMI_OK) {
        MIMI_LOGE("main", "failed to activate default workspace");
        return 1;
    }
    
    /* Load runtime config */
    if (mimi_config_load(config_path) != MIMI_OK) {
        MIMI_LOGW("main", "config load failed; using defaults");
    }
    const mimi_config_t *cfg = mimi_config_get();
    
    /* Set log level - only if --logs flag is provided */
    if (enable_logs) {
        if (log_level) {
            // Command line log level takes precedence over config
            mimi_log_setup(log_level);
        } else {
            // Use log level from config
            mimi_log_setup(cfg->log_level);
        }
    }

    const char *kv_path = "./kv.json";

    if (mimi_workspace_bootstrap(config_path, true) != MIMI_OK) {
        MIMI_LOGE("main", "workspace bootstrap failed");
        return 1;
    }

    
    MIMI_LOGI("main", "workspace=%s kv_path=%s config=%s",
              cfg->workspace[0] ? cfg->workspace : "(default)",
              kv_path, config_path);

    if (mimi_kv_init(kv_path) != MIMI_OK) {
        MIMI_LOGE("main", "failed to init kv");
        return 1;
    }

    if (http_proxy_init() != MIMI_OK) {
        MIMI_LOGW("main", "http_proxy_init failed");
    }

    /* Phase A: core init */
    if (message_bus_init() != MIMI_OK) {
        MIMI_LOGE("main", "message_bus_init failed");
        return 1;
    }
    if (memory_store_init() != MIMI_OK) {
        MIMI_LOGE("main", "memory_store_init failed");
        return 1;
    }
    if (skill_loader_init() != MIMI_OK) {
        MIMI_LOGE("main", "skill_loader_init failed");
        return 1;
    }
    if (session_mgr_init() != MIMI_OK) {
        MIMI_LOGE("main", "session_mgr_init failed");
        return 1;
    }
    if (tool_registry_init() != MIMI_OK) {
        MIMI_LOGE("main", "tool_registry_init failed");
        return 1;
    }
    if (llm_proxy_init() != MIMI_OK) {
        MIMI_LOGE("main", "llm_proxy_init failed");
        return 1;
    }
    if (agent_loop_init() != MIMI_OK) {
        MIMI_LOGE("main", "agent_loop_init failed");
        return 1;
    }
    if (agent_loop_start() != MIMI_OK) {
        MIMI_LOGE("main", "agent_loop_start failed");
        return 1;
    }

    MIMI_LOGI("main", "Core started. Initializing runtime...");

    /* Initialize platform runtime (event loop, timers, etc.). */
    if (mimi_runtime_init() != MIMI_OK) {
        MIMI_LOGE("main", "mimi_runtime_init failed");
        return 1;
    }

    /* Start WebSocket server on top of the OS event loop. */
    if (ws_server_start() != MIMI_OK) {
        MIMI_LOGE("main", "ws_server_start failed");
    }

    /* Optional Telegram bot */
    if (telegram_bot_init() == MIMI_OK) {
        if (telegram_bot_start() != MIMI_OK) {
            MIMI_LOGW("main", "telegram_bot_start failed (token missing?)");
        }
    }

    /* Outbound dispatch */
    mimi_task_create_detached("outbound_dispatch", outbound_dispatch_task, NULL);

    /* Start stdio CLI for debugging */
    if (stdio_cli_start() != MIMI_OK) {
        MIMI_LOGW("main", "stdio cli failed to start");
    }

    if (cron_service_init() != MIMI_OK) {
        MIMI_LOGE("main", "cron_service_init failed");
    } else {
        if (cron_service_start() != MIMI_OK) {
            MIMI_LOGE("main", "cron_service_start failed");
        }
    }
    if (heartbeat_init() != MIMI_OK) {
        MIMI_LOGE("main", "heartbeat_init failed");
    } else {
        if (heartbeat_start() != MIMI_OK) {
            MIMI_LOGE("main", "heartbeat_start failed");
        }
    }

    MIMI_LOGI("main", "Runtime initialized. Entering main event loop...");
    return (mimi_runtime_run() == MIMI_OK) ? 0 : 1;
}

