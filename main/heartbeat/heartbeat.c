#include "heartbeat/heartbeat.h"
#include "config.h"
#include "bus/message_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "log.h"
#include "os/os.h"
#include "fs/fs.h"

static const char *TAG = "heartbeat";

static mimi_timer_handle_t s_timer_handle = NULL;

/* ── Content check ────────────────────────────────────────────── */

/**
 * Check if HEARTBEAT.md has actionable content.
 * Returns true if any line is NOT:
 *   - empty / whitespace-only
 *   - a markdown header (starts with #)
 *   - a completed checkbox (- [x] or * [x])
 */
static bool heartbeat_has_tasks(void)
{
    const mimi_config_t *cfg = mimi_config_get();
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(cfg->heartbeat_file, "r", &f);
    if (err != MIMI_OK) {
        return false;
    }

    char line[256];
    bool found_task = false;

    for (;;) {
        bool eof = false;
        err = mimi_fs_read_line(f, line, sizeof(line), &eof);
        if (err != MIMI_OK) break;
        if (eof) break;
        /* Skip leading whitespace */
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        /* Skip empty lines */
        if (*p == '\0') {
            continue;
        }

        /* Skip markdown headers */
        if (*p == '#') {
            continue;
        }

        /* Skip completed checkboxes: "- [x]" or "* [x]" */
        if ((*p == '-' || *p == '*') && *(p + 1) == ' ' && *(p + 2) == '[') {
            char mark = *(p + 3);
            if ((mark == 'x' || mark == 'X') && *(p + 4) == ']') {
                continue;
            }
        }

        /* Found an actionable line */
        found_task = true;
        break;
    }

    mimi_fs_close(f);
    return found_task;
}

/* ── Send heartbeat to agent ──────────────────────────────────── */

static bool heartbeat_send(void)
{
    if (!heartbeat_has_tasks()) {
        MIMI_LOGD(TAG, "No actionable tasks in HEARTBEAT.md");
        return false;
    }

    const mimi_config_t *cfg = mimi_config_get();
    char prompt[512];
    snprintf(prompt, sizeof(prompt),
             "Read %s and follow any instructions or tasks listed there. "
             "If nothing needs attention, reply with just: HEARTBEAT_OK",
             cfg->heartbeat_file);

    mimi_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, MIMI_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "heartbeat", sizeof(msg.chat_id) - 1);
    msg.content = strdup(prompt);

    if (!msg.content) {
        MIMI_LOGE(TAG, "Failed to allocate heartbeat prompt");
        return false;
    }

    mimi_err_t err = message_bus_push_inbound(&msg);
    if (err != MIMI_OK) {
        MIMI_LOGW(TAG, "Failed to push heartbeat message: %s", mimi_err_to_name(err));
        free(msg.content);
        return false;
    }

    MIMI_LOGI(TAG, "Triggered agent check");
    return true;
}

static void heartbeat_timer_cb(void *arg)
{
    (void)arg;
    heartbeat_send();
}

/* ── Public API ───────────────────────────────────────────────── */

mimi_err_t heartbeat_init(void)
{
    const mimi_config_t *cfg = mimi_config_get();
    MIMI_LOGD(TAG, "Heartbeat service initialized (file: %s, interval: %ds)",
             cfg->heartbeat_file,
             ((cfg->heartbeat_interval_ms > 0) ? cfg->heartbeat_interval_ms : (30 * 60 * 1000)) / 1000);
    return MIMI_OK;
}

mimi_err_t heartbeat_start(void)
{
    if (s_timer_handle != NULL) {
        MIMI_LOGW(TAG, "Heartbeat timer already running");
        return MIMI_OK;
    }

    const mimi_config_t *cfg = mimi_config_get();
    int interval_ms = (cfg->heartbeat_interval_ms > 0) ? cfg->heartbeat_interval_ms : (30 * 60 * 1000);

    mimi_err_t err = mimi_timer_start(interval_ms, true,
                                       heartbeat_timer_cb, NULL,
                                       &s_timer_handle);
    if (err != MIMI_OK) return err;
    MIMI_LOGD(TAG, "Heartbeat started (every %d min)", interval_ms / 60000);
    return MIMI_OK;
}

void heartbeat_stop(void)
{
    if (s_timer_handle != NULL) {
        mimi_timer_stop(&s_timer_handle);
        MIMI_LOGI(TAG, "Heartbeat stopped");
    }
}

bool heartbeat_trigger(void)
{
    return heartbeat_send();
}
