/**
 * Channel Manager Implementation
 */

#include "channels/channel_manager.h"
#include "gateway/gateway_manager.h"
#include "gateway/gateway.h"
#include "log.h"
#include <string.h>

static const char *TAG = "channel_mgr";

/* Channel manager state */
typedef struct {
    bool initialized;
    channel_manager_config_t config;
    channel_t *channels[CHANNEL_MAX_COUNT];
    int count;
} channel_manager_state_t;

static channel_manager_state_t s_state = {
    .initialized = false,
    .config = {
        .max_channels = CHANNEL_MAX_COUNT,
        .auto_start = false,
    },
    .count = 0,
};

mimi_err_t channel_manager_init(void)
{
    return channel_manager_init_with_config(NULL);
}

mimi_err_t channel_manager_init_with_config(const channel_manager_config_t *cfg)
{
    if (s_state.initialized) {
        MIMI_LOGW(TAG, "Channel manager already initialized");
        return MIMI_OK;
    }

    memset(s_state.channels, 0, sizeof(s_state.channels));
    s_state.count = 0;

    if (cfg) {
        s_state.config = *cfg;
        if (s_state.config.max_channels > CHANNEL_MAX_COUNT) {
            s_state.config.max_channels = CHANNEL_MAX_COUNT;
        }
    }

    s_state.initialized = true;
    MIMI_LOGD(TAG, "Channel manager initialized (max_channels=%d, auto_start=%s)",
              s_state.config.max_channels,
              s_state.config.auto_start ? "true" : "false");

    return MIMI_OK;
}

void channel_manager_deinit(void)
{
    if (!s_state.initialized) {
        return;
    }

    /* Stop all channels */
    channel_stop_all();

    /* Clean up state */
    memset(s_state.channels, 0, sizeof(s_state.channels));
    s_state.count = 0;
    s_state.initialized = false;

    MIMI_LOGI(TAG, "Channel manager deinitialized");
}

const channel_manager_config_t* channel_manager_get_config(void)
{
    if (!s_state.initialized) {
        return NULL;
    }
    return &s_state.config;
}

bool channel_manager_is_initialized(void)
{
    return s_state.initialized;
}

mimi_err_t channel_register(channel_t *ch)
{
    if (!s_state.initialized) {
        MIMI_LOGE(TAG, "Channel manager not initialized");
        return MIMI_ERR_INVALID_STATE;
    }

    if (!ch) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (strlen(ch->name) == 0) {
        MIMI_LOGE(TAG, "Channel name is required");
        return MIMI_ERR_INVALID_ARG;
    }

    /* Check if already exists */
    if (channel_find(ch->name)) {
        MIMI_LOGW(TAG, "Channel '%s' already registered", ch->name);
        return MIMI_ERR_INVALID_STATE;
    }

    /* Check capacity */
    if (s_state.count >= s_state.config.max_channels) {
        MIMI_LOGE(TAG, "Channel limit reached (%d)", s_state.config.max_channels);
        return MIMI_ERR_NO_MEM;
    }

    /* Initialize channel */
    if (ch->init) {
        channel_config_t cfg = {
            .name = ch->name,
            .description = ch->description,
            .require_auth = ch->require_auth,
            .max_sessions = ch->max_sessions,
            .user_data = ch->priv_data,
        };
        mimi_err_t err = ch->init(ch, &cfg);
        if (err != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to init channel '%s': %d", ch->name, err);
            return err;
        }
    }

    /* Register */
    s_state.channels[s_state.count++] = ch;
    ch->is_initialized = true;

    MIMI_LOGD(TAG, "Channel '%s' registered (%d/%d)", 
              ch->name, s_state.count, s_state.config.max_channels);

    /* Auto-start */
    if (s_state.config.auto_start && ch->start) {
        mimi_err_t err = ch->start(ch);
        if (err != MIMI_OK) {
            MIMI_LOGW(TAG, "Failed to auto-start channel '%s': %d", ch->name, err);
        } else {
            ch->is_started = true;
        }
    }

    return MIMI_OK;
}

void channel_unregister(channel_t *ch)
{
    if (!s_state.initialized || !ch) {
        return;
    }

    /* Find and remove */
    for (int i = 0; i < s_state.count; i++) {
        if (s_state.channels[i] == ch) {
            /* Stop channel */
            if (ch->is_started && ch->stop) {
                ch->stop(ch);
                ch->is_started = false;
            }

            /* Destroy channel */
            if (ch->destroy) {
                ch->destroy(ch);
            }

            /* Remove */
            for (int j = i; j < s_state.count - 1; j++) {
                s_state.channels[j] = s_state.channels[j + 1];
            }
            s_state.channels[--s_state.count] = NULL;
            ch->is_initialized = false;

            MIMI_LOGI(TAG, "Channel '%s' unregistered (%d/%d)",
                      ch->name, s_state.count, s_state.config.max_channels);
            return;
        }
    }
}

channel_t* channel_find(const char *name)
{
    if (!s_state.initialized || !name) {
        return NULL;
    }

    for (int i = 0; i < s_state.count; i++) {
        if (s_state.channels[i] && 
            strcmp(s_state.channels[i]->name, name) == 0) {
            return s_state.channels[i];
        }
    }

    return NULL;
}

mimi_err_t channel_start_all(void)
{
    if (!s_state.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    MIMI_LOGI(TAG, "Starting all channels (%d)...", s_state.count);

    for (int i = 0; i < s_state.count; i++) {
        channel_t *ch = s_state.channels[i];
        if (ch && !ch->is_started && ch->start) {
            mimi_err_t err = ch->start(ch);
            if (err != MIMI_OK) {
                MIMI_LOGW(TAG, "Failed to start channel '%s': %d", ch->name, err);
            } else {
                ch->is_started = true;
                MIMI_LOGD(TAG, "Channel '%s' started", ch->name);
            }
        }
    }

    return MIMI_OK;
}

void channel_stop_all(void)
{
    if (!s_state.initialized) {
        return;
    }

    MIMI_LOGI(TAG, "Stopping all channels (%d)...", s_state.count);

    for (int i = 0; i < s_state.count; i++) {
        channel_t *ch = s_state.channels[i];
        if (ch && ch->is_started && ch->stop) {
            ch->stop(ch);
            ch->is_started = false;
            MIMI_LOGI(TAG, "Channel '%s' stopped", ch->name);
        }
    }
}

mimi_err_t channel_send(const mimi_msg_t *msg)
{
    if (!s_state.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!msg || !msg->channel[0] || !msg->chat_id[0]) {
        return MIMI_ERR_INVALID_ARG;
    }

    channel_t *ch = channel_find(msg->channel);
    if (!ch) {
        MIMI_LOGW(TAG, "Channel '%s' not found", msg->channel);
        return MIMI_ERR_NOT_FOUND;
    }

    if (!ch->is_started) {
        MIMI_LOGW(TAG, "Channel '%s' not started", msg->channel);
        return MIMI_ERR_INVALID_STATE;
    }

    if (!ch->send_msg) {
        return MIMI_ERR_NOT_SUPPORTED;
    }

    return ch->send_msg(ch, msg);
}

int channel_get_count(void)
{
    if (!s_state.initialized) {
        return 0;
    }
    return s_state.count;
}

channel_t* channel_get_by_index(int index)
{
    if (!s_state.initialized || index < 0 || index >= s_state.count) {
        return NULL;
    }
    return s_state.channels[index];
}

void channel_poll_all(void)
{
    if (!s_state.initialized) {
        return;
    }

    /* Poll all channels */
    for (int i = 0; i < s_state.count; i++) {
        channel_t *ch = s_state.channels[i];
        if (ch && ch->is_started) {
            /* Note: Polling logic should be implemented in each channel */
        }
    }
}

/* External channel init functions */
extern mimi_err_t cli_channel_init(void);
extern mimi_err_t telegram_channel_init(void);
extern mimi_err_t ws_server_channel_init(void);
extern mimi_err_t feishu_channel_init(void);
extern mimi_err_t qq_channel_init(void);

/* External channel instances */
extern channel_t g_cli_channel;
extern channel_t g_telegram_channel;
extern channel_t g_ws_server_channel;
extern channel_t g_feishu_channel;
extern channel_t g_qq_channel;

/* External Router init */
extern mimi_err_t router_init(void);

mimi_err_t channel_system_init(void)
{
    MIMI_LOGI(TAG, "Initializing channel system...");
    
    /* Initialize Router (must be before channels) */
    if (router_init() != MIMI_OK) {
        MIMI_LOGE(TAG, "router_init failed");
        return MIMI_ERR_FAIL;
    }
    MIMI_LOGI(TAG, "Router initialized");

    /* Initialize Channel Manager */
    if (channel_manager_init() != MIMI_OK) {
        MIMI_LOGE(TAG, "channel_manager_init failed");
        return MIMI_ERR_FAIL;
    }

    /* Initialize and register CLI Channel */
    if (cli_channel_init() != MIMI_OK) {
        MIMI_LOGW(TAG, "cli_channel_init failed");
    } else {
        if (channel_register(&g_cli_channel) != MIMI_OK) {
            MIMI_LOGW(TAG, "Failed to register CLI channel");
        }
    }

    /* Initialize and register Telegram Channel */
    if (telegram_channel_init() != MIMI_OK) {
        MIMI_LOGW(TAG, "telegram_channel_init failed");
    } else {
        if (channel_register(&g_telegram_channel) != MIMI_OK) {
            MIMI_LOGW(TAG, "Failed to register Telegram channel");
        }
    }

    /* Initialize and register WebSocket Server Channel */
    if (ws_server_channel_init() != MIMI_OK) {
        MIMI_LOGW(TAG, "ws_server_channel_init failed");
    } else {
        if (channel_register(&g_ws_server_channel) != MIMI_OK) {
            MIMI_LOGW(TAG, "Failed to register WebSocket server channel");
        }
    }

    /* Initialize and register Feishu Channel */
    if (feishu_channel_init() != MIMI_OK) {
        MIMI_LOGW(TAG, "feishu_channel_init failed");
    } else {
        if (channel_register(&g_feishu_channel) != MIMI_OK) {
            MIMI_LOGW(TAG, "Failed to register Feishu channel");
        }
    }

    /* Initialize and register QQ Channel */
    if (qq_channel_init() != MIMI_OK) {
        MIMI_LOGW(TAG, "qq_channel_init failed");
    } else {
        if (channel_register(&g_qq_channel) != MIMI_OK) {
            MIMI_LOGW(TAG, "Failed to register QQ channel");
        }
    }

    MIMI_LOGD(TAG, "Channel system initialized (%d channels)", channel_get_count());
    return MIMI_OK;
}

mimi_err_t channel_system_start(void)
{
    MIMI_LOGI(TAG, "Starting channel system...");
    return channel_start_all();
}

void channel_system_stop(void)
{
    MIMI_LOGI(TAG, "Stopping channel system...");
    channel_stop_all();
}

/* Legacy function - kept for backward compatibility */
mimi_err_t channel_system_auto_init(void)
{
    if (channel_system_init() != MIMI_OK) {
        return MIMI_ERR_FAIL;
    }
    return channel_system_start();
}
