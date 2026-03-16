/**
 * Router Implementation
 * 
 * Unified routing for commands and chat messages from all gateways
 */

#include "router/router.h"
#include "commands/command.h"
#include "bus/message_bus.h"
#include "channels/channel_manager.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "router";

/* Gateway to Channel mapping */
typedef struct {
    char gateway_name[32];
    char channel_name[32];
    bool active;
} gateway_channel_mapping_t;

static struct {
    gateway_channel_mapping_t mappings[ROUTER_MAX_MAPPINGS];
    int count;
    bool initialized;
} g_router = {0};

mimi_err_t router_init(void)
{
    if (g_router.initialized) {
        MIMI_LOGW(TAG, "Router already initialized");
        return MIMI_OK;
    }
    
    memset(&g_router, 0, sizeof(g_router));
    g_router.initialized = true;
    
    MIMI_LOGD(TAG, "Router initialized");
    return MIMI_OK;
}

mimi_err_t router_register_mapping(const char *gateway_name, 
                                   const char *channel_name)
{
    if (!g_router.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!gateway_name || !channel_name) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (g_router.count >= ROUTER_MAX_MAPPINGS) {
        MIMI_LOGE(TAG, "Mapping registry full");
        return MIMI_ERR_NO_MEM;
    }
    
    /* Check if already exists */
    for (int i = 0; i < g_router.count; i++) {
        if (strcmp(g_router.mappings[i].gateway_name, gateway_name) == 0) {
            /* Update existing */
            strncpy(g_router.mappings[i].channel_name, channel_name, 
                    sizeof(g_router.mappings[i].channel_name) - 1);
            g_router.mappings[i].channel_name[sizeof(g_router.mappings[i].channel_name) - 1] = '\0';
            MIMI_LOGI(TAG, "Updated mapping: %s -> %s", gateway_name, channel_name);
            return MIMI_OK;
        }
    }
    
    /* Add new mapping */
    gateway_channel_mapping_t *mapping = &g_router.mappings[g_router.count++];
    strncpy(mapping->gateway_name, gateway_name, sizeof(mapping->gateway_name) - 1);
    mapping->gateway_name[sizeof(mapping->gateway_name) - 1] = '\0';
    
    strncpy(mapping->channel_name, channel_name, sizeof(mapping->channel_name) - 1);
    mapping->channel_name[sizeof(mapping->channel_name) - 1] = '\0';
    
    mapping->active = true;
    
    MIMI_LOGD(TAG, "Registered mapping: %s -> %s (%d/%d)",
              gateway_name, channel_name, g_router.count, ROUTER_MAX_MAPPINGS);
    return MIMI_OK;
}

mimi_err_t router_unregister_mapping(const char *gateway_name)
{
    if (!g_router.initialized || !gateway_name) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    for (int i = 0; i < g_router.count; i++) {
        if (strcmp(g_router.mappings[i].gateway_name, gateway_name) == 0) {
            /* Mark as inactive */
            g_router.mappings[i].active = false;
            MIMI_LOGI(TAG, "Unregistered mapping for gateway '%s'", gateway_name);
            return MIMI_OK;
        }
    }
    
    return MIMI_ERR_NOT_FOUND;
}

const char* router_find_channel(const char *gateway_name)
{
    if (!g_router.initialized || !gateway_name) {
        return NULL;
    }
    
    for (int i = 0; i < g_router.count; i++) {
        if (g_router.mappings[i].active && 
            strcmp(g_router.mappings[i].gateway_name, gateway_name) == 0) {
            return g_router.mappings[i].channel_name;
        }
    }
    
    return NULL;
}

bool router_is_command(const char *content)
{
    if (!content || !*content) {
        return false;
    }
    
    /* Skip leading whitespace */
    while (*content && isspace((unsigned char)*content)) {
        content++;
    }
    
    return content[0] == '/';
}

static mimi_err_t router_execute_command(gateway_t *gw,
                                         const char *channel_name,
                                         const char *session_id,
                                         const char *command)
{
    if (!gw || !channel_name || !session_id || !command) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Prepare command context */
    command_context_t ctx = {
        .channel = channel_name,
        .session_id = session_id,
        .user_id = session_id,  /* Use session_id as user_id for simplicity */
        .user_data = gw,
    };
    
    /* Execute command */
    char output[ROUTER_OUTPUT_MAX_LEN];
    int ret = command_execute(command, &ctx, output, sizeof(output));
    
    /* Send response back through gateway */
    if (output[0] != '\0') {
        mimi_err_t err = gateway_send(gw, session_id, output);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to send command response: %d", err);
            return err;
        }
    }
    
    (void)ret;  /* Command execution result doesn't affect routing */
    return MIMI_OK;
}

static mimi_err_t router_send_to_agent(const char *channel_name,
                                       const char *session_id,
                                       const char *content)
{
    if (!channel_name || !session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    /* Create message */
    mimi_msg_t msg = {0};
    
    strncpy(msg.channel, channel_name, sizeof(msg.channel) - 1);
    msg.channel[sizeof(msg.channel) - 1] = '\0';
    
    strncpy(msg.chat_id, session_id, sizeof(msg.chat_id) - 1);
    msg.chat_id[sizeof(msg.chat_id) - 1] = '\0';
    
    msg.content = strdup(content);
    if (!msg.content) {
        return MIMI_ERR_NO_MEM;
    }
    
    /* Push to message bus */
    mimi_err_t err = message_bus_push_inbound(&msg);
    if (err != MIMI_OK) {
        free(msg.content);
        return err;
    }
    
    return MIMI_OK;
}

mimi_err_t router_handle(gateway_t *gw, 
                         const char *session_id, 
                         const char *content)
{
    if (!gw || !session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    if (!g_router.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    /* Find channel for this gateway */
    const char *channel_name = router_find_channel(gw->name);
    if (!channel_name) {
        MIMI_LOGE(TAG, "No channel mapping for gateway '%s'", gw->name);
        return MIMI_ERR_NOT_FOUND;
    }
    
    MIMI_LOGD(TAG, "Input from %s/%s: %.40s...", channel_name, session_id, content);

    /* Determine if command or chat message */
    if (router_is_command(content)) {
        MIMI_LOGI(TAG, "Routing command from %s: %.40s...", channel_name, content);
        return router_execute_command(gw, channel_name, session_id, content);
    } else {
        MIMI_LOGI(TAG, "Routing chat message from %s to Agent", channel_name);
        return router_send_to_agent(channel_name, session_id, content);
    }
}

mimi_err_t router_handle_telegram(const char *session_id,
                                  const char *content)
{
    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!g_router.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    const char *channel_name = "telegram";

    MIMI_LOGD(TAG, "Telegram input from %s: %.40s...", session_id, content);

    /* Determine if command or chat message */
    if (router_is_command(content)) {
        MIMI_LOGI(TAG, "Routing Telegram command: %.40s...", content);

        /* Execute command without gateway output (Telegram has its own send) */
        char output[ROUTER_OUTPUT_MAX_LEN];
        command_context_t ctx = {
            .channel = channel_name,
            .session_id = session_id,
            .user_id = session_id,
            .user_data = NULL
        };
        int ret = command_execute(content, &ctx, output, sizeof(output));

        if (ret == 0) {
            /* Send response via Telegram Channel */
            mimi_msg_t msg = {0};
            strncpy(msg.channel, channel_name, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, session_id, sizeof(msg.chat_id) - 1);
            msg.type = MIMI_MSG_TYPE_TEXT;
            msg.content = strdup(output);
            if (msg.content) {
                channel_send(&msg);
                free(msg.content);
            }
        }
        return MIMI_OK;
    } else {
        MIMI_LOGI(TAG, "Routing Telegram chat message to Agent");
        return router_send_to_agent(channel_name, session_id, content);
    }
}

mimi_err_t router_handle_feishu(const char *session_id,
                                const char *content)
{
    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!g_router.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    const char *channel_name = "feishu";

    MIMI_LOGD(TAG, "Feishu input from %s: %.40s...", session_id, content);

    /* Determine if command or chat message */
    if (router_is_command(content)) {
        MIMI_LOGI(TAG, "Routing Feishu command: %.40s...", content);

        /* Execute command without gateway output (Feishu has its own send) */
        char output[ROUTER_OUTPUT_MAX_LEN];
        command_context_t ctx = {
            .channel = channel_name,
            .session_id = session_id,
            .user_id = session_id,
            .user_data = NULL
        };
        int ret = command_execute(content, &ctx, output, sizeof(output));

        if (ret == 0) {
            /* Send response via Feishu Channel */
            mimi_msg_t msg = {0};
            strncpy(msg.channel, channel_name, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, session_id, sizeof(msg.chat_id) - 1);
            msg.type = MIMI_MSG_TYPE_TEXT;
            msg.content = strdup(output);
            if (msg.content) {
                channel_send(&msg);
                free(msg.content);
            }
        }
        return MIMI_OK;
    } else {
        MIMI_LOGI(TAG, "Routing Feishu chat message to Agent");
        return router_send_to_agent(channel_name, session_id, content);
    }
}

mimi_err_t router_handle_qq(const char *session_id,
                            const char *content)
{
    if (!session_id || !content) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!g_router.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    const char *channel_name = "qq";

    MIMI_LOGD(TAG, "QQ input from %s: %.40s...", session_id, content);

    /* Determine if command or chat message */
    if (router_is_command(content)) {
        MIMI_LOGI(TAG, "Routing QQ command: %.40s...", content);

        /* Execute command without gateway output (QQ has its own send) */
        char output[ROUTER_OUTPUT_MAX_LEN];
        command_context_t ctx = {
            .channel = channel_name,
            .session_id = session_id,
            .user_id = session_id,
            .user_data = NULL
        };
        int ret = command_execute(content, &ctx, output, sizeof(output));

        if (ret == 0) {
            /* Send response via QQ Channel */
            mimi_msg_t msg = {0};
            strncpy(msg.channel, channel_name, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, session_id, sizeof(msg.chat_id) - 1);
            msg.type = MIMI_MSG_TYPE_TEXT;
            msg.content = strdup(output);
            if (msg.content) {
                channel_send(&msg);
                free(msg.content);
            }
        }
        return MIMI_OK;
    } else {
        MIMI_LOGI(TAG, "Routing QQ chat message to Agent");
        return router_send_to_agent(channel_name, session_id, content);
    }
}