#include "log.h"
#include "fs/fs.h"
#include "kv.h"
#include "mimi_time.h"
#include "os/os.h"
#include "runtime.h"
#include "platform/path_utils.h"

#include "config.h"
#include "workspace_bootstrap.h"

#ifdef _WIN32
#include <windows.h>
#endif

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

/* Channel and Command system */
#include "channels/channel_manager.h"
#include "commands/command.h"

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
        
        /* Use Channel Manager to route messages */
        mimi_err_t err = channel_send(msg.channel, msg.chat_id, msg.content ? msg.content : "");
        if (err != MIMI_OK) {
            /* Fallback for channels not yet migrated */
            if (strcmp(msg.channel, MIMI_CHAN_CLI) == 0) {
                mimi_tty_printf("[cli:%s] %s\n", msg.chat_id, msg.content ? msg.content : "");
            } else {
                MIMI_LOGW("dispatch", "Failed to send to channel %s: %s", 
                          msg.channel, mimi_err_to_name(err));
            }
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
#ifdef _WIN32
    /* Set Windows console to UTF-8 mode for proper Unicode/Chinese support */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    bool enable_logs = false;
    const char *config_path = NULL;
    const char *log_level = NULL;

    MIMI_LOGI("main", "MimiClaw starting");

    parse_args(argc, argv, &enable_logs, &config_path, &log_level);

    char config_buf[512] = {0};
    if (!config_path) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || home[0] == '\0') {
            home = getenv("USERPROFILE");
        }
#endif
        if (home && home[0] != '\0') {
            mimi_path_join_multi(config_buf, sizeof(config_buf), home, ".mimiclaw", "config.json", NULL);
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
#ifdef _WIN32
    if (!home || home[0] == '\0') {
        home = getenv("USERPROFILE");
    }
#endif
    if (home && home[0] != '\0') {
        mimi_path_join_multi(default_workspace, sizeof(default_workspace), home, ".mimiclaw", "workspace", NULL);
    } else {
        strncpy(default_workspace, "./spiffs", sizeof(default_workspace) - 1);
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

    /* Initialize platform runtime early (needed for mongoose logging and WS) */
    if (mimi_runtime_init() != MIMI_OK) {
        MIMI_LOGE("main", "mimi_runtime_init failed");
        return 1;
    }

    /* Auto-initialize Command System (registers all built-in commands) */
    if (command_system_auto_init() != 0) {
        MIMI_LOGE("main", "command_system_auto_init failed");
        return 1;
    }
    MIMI_LOGI("main", "Command system initialized (%d commands)", command_get_count());

    /* Auto-initialize Channel System (registers and starts all channels) */
    if (channel_system_auto_init() != MIMI_OK) {
        MIMI_LOGE("main", "channel_system_auto_init failed");
        return 1;
    }
    MIMI_LOGI("main", "Channel system initialized");

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

    MIMI_LOGI("main", "Core started. Runtime initialized.");

    /* Channels are now initialized and started via Channel Manager below */

    /* Outbound dispatch */
    mimi_task_create_detached("outbound_dispatch", outbound_dispatch_task, NULL);

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

