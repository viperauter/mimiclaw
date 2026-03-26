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
#include "os/os.h"
#include "runtime.h"
#include "path_utils.h"

#include "config.h"
#include "config_view.h"
#include "workspace_bootstrap.h"

#include "bus/message_bus.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "skills/skill_loader.h"
#include "tools/tool_registry.h"
#include "agent/agent_async_loop.h"
#include "mimi_config.h"

#if MIMI_ENABLE_SUBAGENT
#include "agent/subagent/subagent_config.h"
#include "agent/subagent/subagent_manager.h"
#endif

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
static bool s_gateway_mode = false;

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

        /* Handle control messages differently */
        if (msg.type == MIMI_MSG_TYPE_CONTROL) {
            channel_t *ch = channel_find(msg.channel);
            if (ch && ch->send_control) {
                mimi_err_t err = ch->send_control(ch, msg.chat_id, msg.control_type,
                                                      msg.request_id, msg.target, msg.data);
                if (err != MIMI_OK) {
                    MIMI_LOGW("dispatch", "Failed to send control to channel %s: %s",
                              msg.channel, mimi_err_to_name(err));
                }
            } else {
                MIMI_LOGW("dispatch", "Channel %s does not support control messages",
                          msg.channel);
            }
            free(msg.content);
            continue;
        }
        
        /* Non-control messages: prefer extended send_msg() when available. */
        channel_t *ch = channel_find(msg.channel);
        mimi_err_t err = MIMI_ERR_NOT_FOUND;
        if (ch && ch->send_msg) {
            err = ch->send_msg(ch, &msg);
        } else {
            /* Fallback: treat as plain text. */
            err = channel_send(&msg);
            if (err != MIMI_OK) {
                if (strcmp(msg.channel, MIMI_CHAN_CLI) == 0) {
                    mimi_tty_printf("[cli:%s] %s\n", msg.chat_id, msg.content ? msg.content : "");
                } else {
                    MIMI_LOGW("dispatch", "Failed to send to channel %s: %s",
                              msg.channel, mimi_err_to_name(err));
                }
            }
        }
        free(msg.content);
    }
}

mimi_err_t app_init(const char *config_path,
                    bool enable_logs,
                    const char *log_level,
                    bool gateway_mode,
                    const char *log_file_path)
{
    if (s_app_initialized) {
        return MIMI_OK;
    }

    s_gateway_mode = gateway_mode;

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

    /* Load config first so we know the log level */
    if (mimi_config_load(cfg_path) != MIMI_OK) {
        MIMI_LOGW("app", "config load failed; using defaults");
    }
    mimi_cfg_obj_t defaults = mimi_cfg_get_obj(mimi_cfg_section("agents"), "defaults");
    mimi_cfg_obj_t logging = mimi_cfg_section("logging");
    mimi_cfg_obj_t feishu = mimi_cfg_named("channels", "feishu");
    const char *log_level_cfg = mimi_cfg_get_str(logging, "level", "info");
    const char *model = mimi_cfg_get_str(defaults, "model", "");
    const char *provider = mimi_cfg_get_str(defaults, "provider", "");
    bool feishu_enabled = mimi_cfg_get_bool(feishu, "enabled", false);
    const char *feishu_app_id = mimi_cfg_get_str(feishu, "appId", "");
    const char *workspace = mimi_cfg_get_str(defaults, "workspace", "");

    bool log_to_file_cfg = mimi_cfg_get_bool(logging, "toFile", false);
    bool log_to_stderr_cfg = mimi_cfg_get_bool(logging, "toStderr", true);
    /* Directory is fixed to workspace/logs (user cannot override). */
    const char *log_dir_cfg = "logs";
    const char *log_file_cfg = mimi_cfg_get_str(logging, "file", "mimiclaw.log");
    int log_max_file_bytes_cfg = mimi_cfg_get_int(logging, "maxFileBytes", 5 * 1024 * 1024);
    int log_max_files_cfg = mimi_cfg_get_int(logging, "maxFiles", 3);

    /* CLI -l/-f or config logging.toFile: enable log pipeline (otherwise config logging is ignored). */
    bool want_log_output =
        enable_logs || log_to_file_cfg || (log_file_path && log_file_path[0]);

    /* Setup logging after config load (config may specify log level) */
    if (want_log_output) {
        mimi_log_setup(log_level ? log_level : (log_level_cfg && log_level_cfg[0] ? log_level_cfg : "info"));
    }

    /* File logging before bootstrap and routine MIMI_LOG lines so toStderr is honored. */
    if (want_log_output && (log_to_file_cfg || (log_file_path && log_file_path[0]))) {
        char resolved_log_path[1024];
        resolved_log_path[0] = '\0';

        if (log_file_path && log_file_path[0]) {
            if (mimi_path_is_absolute(log_file_path)) {
                strncpy(resolved_log_path, log_file_path, sizeof(resolved_log_path) - 1);
                resolved_log_path[sizeof(resolved_log_path) - 1] = '\0';
            } else if (mimi_path_join(workspace, log_file_path, resolved_log_path, sizeof(resolved_log_path)) != 0) {
                MIMI_LOGW("app", "Failed to resolve --log-file path: %s", log_file_path);
            }
        } else {
            char log_dir_path[768];
            if (mimi_path_join(workspace, log_dir_cfg, log_dir_path, sizeof(log_dir_path)) == 0 &&
                mimi_path_join(log_dir_path, log_file_cfg, resolved_log_path, sizeof(resolved_log_path)) == 0) {
                /* resolved */
            } else {
                MIMI_LOGW("app", "Failed to resolve logging.dir/logging.file path");
            }
        }

        if (resolved_log_path[0] != '\0') {
            mimi_log_set_rotation(log_max_file_bytes_cfg, log_max_files_cfg);
            mimi_err_t lerr = mimi_log_set_output_file(resolved_log_path, log_to_stderr_cfg);
            if (lerr != MIMI_OK) {
                MIMI_LOGW("app", "Failed to enable file logging (%s): %s",
                          resolved_log_path, mimi_err_to_name(lerr));
            } else {
                MIMI_LOGI("app", "File logging enabled: %s", resolved_log_path);
            }
        }
    }

    if (mimi_workspace_bootstrap(cfg_path, true) != MIMI_OK) {
        MIMI_LOGE("app", "workspace bootstrap failed");
        return MIMI_ERR_FAIL;
    }

    /* Print OS backend version */
    MIMI_LOGI("app", "OS backend: %s", mimi_os_get_version());
    MIMI_LOGI("app", "Config: model=%s provider=%s", model, provider);

    MIMI_LOGD("app", "Config loaded: feishu_enabled=%d, feishu_app_id=%s",
              feishu_enabled, (feishu_app_id && feishu_app_id[0]) ? feishu_app_id : "(empty)");

    MIMI_LOGD("app", "workspace=%s config=%s",
              (workspace && workspace[0]) ? workspace : "(default)", cfg_path);

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
    MIMI_LOGD("app", "Command system initialized (%d commands)", command_get_count());

    /* Initialize Gateway System */
    if (gateway_system_init(s_gateway_mode) != MIMI_OK) {
        MIMI_LOGE("app", "gateway_system_init failed");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGD("app", "Gateway system initialized");

    /* Initialize Channel System */
    if (channel_system_init() != MIMI_OK) {
        MIMI_LOGE("app", "channel_system_init failed");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGD("app", "Channel system initialized");

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

    /* Initialize subagent configs (if any) after tools are registered (needed for tools_json filtering). */
#if MIMI_ENABLE_SUBAGENT
    subagent_config_init();
    /* Manager is used by the subagents tool and may be used by internal code. */
    subagent_manager_init();
#endif

    if (llm_proxy_init() != MIMI_OK) {
        MIMI_LOGE("app", "llm_proxy_init failed");
        return MIMI_ERR_FAIL;
    }
    if (agent_async_loop_init() != MIMI_OK) {
        MIMI_LOGE("app", "agent_async_loop_init failed");
        return MIMI_ERR_FAIL;
    }

    s_app_initialized = true;
    MIMI_LOGD("app", "Application initialized");

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

    if (agent_async_loop_start() != MIMI_OK) {
        MIMI_LOGE("app", "agent_async_loop_start failed");
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

    /* Start Runtime (event loop in separate thread) FIRST
     * HTTP and WebSocket need the event loop running */
    if (mimi_runtime_start() != MIMI_OK) {
        MIMI_LOGE("app", "Failed to start runtime");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGD("app", "Runtime started");

    /* Start Gateway System */
    if (gateway_system_start() != MIMI_OK) {
        MIMI_LOGE("app", "Failed to start gateway system");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGD("app", "Gateway system started");

    /* Start Channel System */
    if (channel_system_start() != MIMI_OK) {
        MIMI_LOGE("app", "Failed to start channel system");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGD("app", "Channel system started");

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

    /* Stop agent loop */
    agent_async_loop_stop();

    /* Stop channel system */
    channel_system_stop();

    /* Stop gateway system */
    gateway_system_stop();

    /* Stop runtime last: other subsystems may still need the event loop to close cleanly */
    mimi_runtime_stop();

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
    
    /* Deinitialize tool registry */
    tool_registry_deinit();

    s_app_initialized = false;
    MIMI_LOGI("app", "Application destroyed");

    /* Close the log output after the final log line. */
    mimi_log_close_output_file();
}
