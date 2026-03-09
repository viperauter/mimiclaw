/**
 * @file app.c
 * @brief Application layer implementation - platform-agnostic application core
 *
 * This module implements the application layer interface defined in app.h.
 * It provides the core application logic that is independent of the underlying
 * platform. All platform-specific code should be confined to the main/ directory.
 */

#include "app.h"
#include "log.h"
#include "fs/fs.h"
#include "kv.h"
#include "mimi_err.h"
#include "mimi_time.h"
#include "runtime.h"
#include "platform/path_utils.h"

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

#include "channels/channel_manager.h"
#include "commands/command.h"
#include "gateway/gateway_manager.h"
#include "os/os.h"

#include <stdlib.h>
#include <string.h>

static bool s_app_initialized = false;
static bool s_app_started = false;

// Cleanup callback function
static void app_cleanup(void)
{
    /* Stop all gateways to restore terminal settings */
    gateway_manager_stop_all();
    gateway_manager_destroy_all();
}

static void outbound_dispatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != MIMI_OK) {
            continue;
        }

        mimi_err_t err = channel_send(msg.channel, msg.chat_id, msg.content ? msg.content : "");
        if (err != MIMI_OK) {
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

mimi_err_t app_init(const char *config_path, bool enable_logs, const char *log_level)
{
    if (s_app_initialized) {
        return MIMI_OK;
    }

    const char *cfg_path = config_path;
    if (!cfg_path) {
        cfg_path = "./config.json";
    }

    if (mimi_fs_init() != MIMI_OK) {
        MIMI_LOGE("app", "VFS initialization failed");
        return MIMI_ERR_FAIL;
    }

    extern void posix_fs_register(void);
    posix_fs_register();

    char default_workspace[512];
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        mimi_path_join_multi(default_workspace, sizeof(default_workspace),
                            home, ".mimiclaw", "workspace", NULL);
    } else {
        strncpy(default_workspace, "./spiffs", sizeof(default_workspace) - 1);
    }
    mimi_err_t err = mimi_fs_workspace_create("default", default_workspace);
    if (err != MIMI_OK && err != MIMI_ERR_FAIL) {
        MIMI_LOGE("app", "failed to create default workspace");
        return MIMI_ERR_FAIL;
    }
    if (mimi_fs_workspace_activate("default") != MIMI_OK) {
        MIMI_LOGE("app", "failed to activate default workspace");
        return MIMI_ERR_FAIL;
    }

    if (enable_logs) {
        mimi_log_setup(log_level ? log_level : "info");
    }

    if (mimi_config_load(cfg_path) != MIMI_OK) {
        MIMI_LOGW("app", "config load failed; using defaults");
    }
    const mimi_config_t *cfg = mimi_config_get();

    if (enable_logs && !log_level) {
        mimi_log_setup(cfg->log_level);
    }

    MIMI_LOGI("app", "Config loaded: feishu_enabled=%d, feishu_app_id=%s",
              cfg->feishu_enabled, cfg->feishu_app_id[0] ? cfg->feishu_app_id : "(empty)");

    if (mimi_workspace_bootstrap(cfg_path, true) != MIMI_OK) {
        MIMI_LOGE("app", "workspace bootstrap failed");
        return MIMI_ERR_FAIL;
    }

    MIMI_LOGI("app", "workspace=%s config=%s",
              cfg->workspace[0] ? cfg->workspace : "(default)", cfg_path);

    if (mimi_kv_init("./kv.json") != MIMI_OK) {
        MIMI_LOGE("app", "failed to init kv");
        return MIMI_ERR_FAIL;
    }

    if (http_proxy_init() != MIMI_OK) {
        MIMI_LOGW("app", "http_proxy_init failed");
    }

    if (message_bus_init() != MIMI_OK) {
        MIMI_LOGE("app", "message_bus_init failed");
        return MIMI_ERR_FAIL;
    }

    if (mimi_runtime_init() != MIMI_OK) {
        MIMI_LOGE("app", "mimi_runtime_init failed");
        return MIMI_ERR_FAIL;
    }

    if (command_system_auto_init() != 0) {
        MIMI_LOGE("app", "command_system_auto_init failed");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGI("app", "Command system initialized (%d commands)", command_get_count());

    /* Initialize Gateway System */
    if (gateway_system_init() != MIMI_OK) {
        MIMI_LOGE("app", "gateway_system_init failed");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGI("app", "Gateway system initialized");

    /* Initialize Channel System */
    if (channel_system_init() != MIMI_OK) {
        MIMI_LOGE("app", "channel_system_init failed");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGI("app", "Channel system initialized");

    if (memory_store_init() != MIMI_OK) {
        MIMI_LOGE("app", "memory_store_init failed");
        return MIMI_ERR_FAIL;
    }
    if (skill_loader_init() != MIMI_OK) {
        MIMI_LOGE("app", "skill_loader_init failed");
        return MIMI_ERR_FAIL;
    }
    if (session_mgr_init() != MIMI_OK) {
        MIMI_LOGE("app", "session_mgr_init failed");
        return MIMI_ERR_FAIL;
    }
    if (tool_registry_init() != MIMI_OK) {
        MIMI_LOGE("app", "tool_registry_init failed");
        return MIMI_ERR_FAIL;
    }
    if (llm_proxy_init() != MIMI_OK) {
        MIMI_LOGE("app", "llm_proxy_init failed");
        return MIMI_ERR_FAIL;
    }
    if (agent_loop_init() != MIMI_OK) {
        MIMI_LOGE("app", "agent_loop_init failed");
        return MIMI_ERR_FAIL;
    }

    s_app_initialized = true;
    MIMI_LOGI("app", "Application initialized");

    return MIMI_OK;
}

mimi_err_t app_start(void)
{
    if (!s_app_initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_app_started) {
        return MIMI_OK;
    }

    if (agent_loop_start() != MIMI_OK) {
        MIMI_LOGE("app", "agent_loop_start failed");
        return MIMI_ERR_FAIL;
    }

    mimi_task_create_detached("outbound_dispatch", outbound_dispatch_task, NULL);

    if (cron_service_init() != MIMI_OK) {
        MIMI_LOGW("app", "cron_service_init failed");
    } else {
        if (cron_service_start() != MIMI_OK) {
            MIMI_LOGW("app", "cron_service_start failed");
        }
    }

    if (heartbeat_init() != MIMI_OK) {
        MIMI_LOGW("app", "heartbeat_init failed");
    } else {
        if (heartbeat_start() != MIMI_OK) {
            MIMI_LOGW("app", "heartbeat_start failed");
        }
    }

    /* Start Gateway System */
    if (gateway_system_start() != MIMI_OK) {
        MIMI_LOGE("app", "Failed to start gateway system");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGI("app", "Gateway system started");

    /* Start Channel System */
    if (channel_system_start() != MIMI_OK) {
        MIMI_LOGE("app", "Failed to start channel system");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGI("app", "Channel system started");

    /* Start Runtime (event loop in separate thread) */
    if (mimi_runtime_start() != MIMI_OK) {
        MIMI_LOGE("app", "Failed to start runtime");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGI("app", "Runtime started");

    s_app_started = true;
    MIMI_LOGI("app", "Application started");

    return MIMI_OK;
}

mimi_err_t app_run(void)
{
    if (!s_app_started) {
        return MIMI_ERR_INVALID_STATE;
    }

    MIMI_LOGI("app", "Waiting for runtime...");

    /* Wait for runtime to finish (blocking) */
    while (mimi_runtime_is_running()) {
        mimi_sleep_ms(100);
    }

    MIMI_LOGI("app", "Runtime stopped");
    return MIMI_OK;
}

void app_stop(void)
{
    if (!s_app_started) {
        return;
    }

    MIMI_LOGI("app", "Stopping application...");

    /* Stop runtime first */
    mimi_runtime_stop();

    /* Stop channel system */
    channel_system_stop();

    /* Stop gateway system */
    gateway_system_stop();

    /* Cleanup */
    app_cleanup();

    s_app_started = false;
    MIMI_LOGI("app", "Application stopped");
}

void app_destroy(void)
{
    if (!s_app_initialized) {
        return;
    }

    MIMI_LOGI("app", "Destroying application...");

    /* Deinitialize runtime */
    mimi_runtime_deinit();

    s_app_initialized = false;
    MIMI_LOGI("app", "Application destroyed");
}
