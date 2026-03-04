#include "platform/log.h"
#include "platform/fs.h"
#include "platform/kv.h"
#include "platform/time.h"
#include "platform/os.h"

#include "mimi_config.h"

#include "bus/message_bus.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "skills/skill_loader.h"
#include "tools/tool_registry.h"
#include "agent/agent_loop.h"

#include "llm/llm_proxy.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "gateway/ws_server.h"
#include "telegram/telegram_bot.h"

#include "cli/stdio_cli_posix.h"

#include "mongoose.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
            printf("[cli:%s] %s\n", msg.chat_id, msg.content ? msg.content : "");
            fflush(stdout);
        } else {
            MIMI_LOGW("dispatch", "Unhandled channel %s for chat %s", msg.channel, msg.chat_id);
        }
        free(msg.content);
    }
}

int main(int argc, char **argv)
{
    const char *spiffs_dir = (argc > 1) ? argv[1] : "./spiffs";
    const char *kv_path = (argc > 2) ? argv[2] : "./kv.json";

    MIMI_LOGI("posix", "MimiClaw POSIX starting");
    MIMI_LOGI("posix", "spiffs_dir=%s kv_path=%s", spiffs_dir, kv_path);

    if (mimi_fs_set_base(spiffs_dir) != MIMI_OK) {
        MIMI_LOGE("posix", "failed to set fs base");
        return 1;
    }
    if (mimi_kv_init(kv_path) != MIMI_OK) {
        MIMI_LOGE("posix", "failed to init kv");
        return 1;
    }

    /* Phase A: core init */
    if (message_bus_init() != MIMI_OK) {
        MIMI_LOGE("posix", "message_bus_init failed");
        return 1;
    }
    if (memory_store_init() != MIMI_OK) {
        MIMI_LOGE("posix", "memory_store_init failed");
        return 1;
    }
    if (skill_loader_init() != MIMI_OK) {
        MIMI_LOGE("posix", "skill_loader_init failed");
        return 1;
    }
    if (session_mgr_init() != MIMI_OK) {
        MIMI_LOGE("posix", "session_mgr_init failed");
        return 1;
    }
    if (tool_registry_init() != MIMI_OK) {
        MIMI_LOGE("posix", "tool_registry_init failed");
        return 1;
    }
    if (llm_proxy_init() != MIMI_OK) {
        MIMI_LOGE("posix", "llm_proxy_init failed");
        return 1;
    }
    if (agent_loop_init() != MIMI_OK) {
        MIMI_LOGE("posix", "agent_loop_init failed");
        return 1;
    }
    if (agent_loop_start() != MIMI_OK) {
        MIMI_LOGE("posix", "agent_loop_start failed");
        return 1;
    }

    MIMI_LOGI("posix", "Core started. Running event loop...");

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    /* Provide timers with access to mgr */
    cron_service_set_mgr(&mgr);
    heartbeat_set_mgr(&mgr);

    /* Start WebSocket server */
    ws_server_set_mgr(&mgr);
    if (ws_server_start() != MIMI_OK) {
        MIMI_LOGE("posix", "ws_server_start failed");
    }

    /* Optional Telegram bot */
    if (telegram_bot_init() == MIMI_OK) {
        if (telegram_bot_start() != MIMI_OK) {
            MIMI_LOGW("posix", "telegram_bot_start failed (token missing?)");
        }
    }

    /* Outbound dispatch */
    mimi_task_create_detached("outbound_dispatch", outbound_dispatch_task, NULL);

    /* Start stdio CLI for debugging */
    if (stdio_cli_start() != MIMI_OK) {
        MIMI_LOGW("posix", "stdio cli failed to start");
    }

    if (cron_service_init() != MIMI_OK) {
        MIMI_LOGE("posix", "cron_service_init failed");
    } else {
        if (cron_service_start() != MIMI_OK) {
            MIMI_LOGE("posix", "cron_service_start failed");
        }
    }
    if (heartbeat_init() != MIMI_OK) {
        MIMI_LOGE("posix", "heartbeat_init failed");
    } else {
        if (heartbeat_start() != MIMI_OK) {
            MIMI_LOGE("posix", "heartbeat_start failed");
        }
    }

    for (;;) mg_mgr_poll(&mgr, 100);
    mg_mgr_free(&mgr);
    return 0;
}

