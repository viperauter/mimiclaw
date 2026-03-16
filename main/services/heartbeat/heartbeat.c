#include "heartbeat/heartbeat.h"
#include "config.h"
#include "config_view.h"
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
    mimi_cfg_obj_t files = mimi_cfg_section("files");
    const char *heartbeat_file = mimi_cfg_get_str(files, "heartbeatFile", "HEARTBEAT.md");
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(heartbeat_file, "r", &f);
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

    mimi_cfg_obj_t files = mimi_cfg_section("files");
    const char *heartbeat_file = mimi_cfg_get_str(files, "heartbeatFile", "HEARTBEAT.md");
    char prompt[512];
    snprintf(prompt, sizeof(prompt),
             "Read %s and follow any instructions or tasks listed there. "
             "If nothing needs attention, reply with just: HEARTBEAT_OK",
             heartbeat_file);

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
    mimi_cfg_obj_t files = mimi_cfg_section("files");
    mimi_cfg_obj_t internal = mimi_cfg_section("internal");
    const char *heartbeat_file = mimi_cfg_get_str(files, "heartbeatFile", "HEARTBEAT.md");
    int interval_ms = mimi_cfg_get_int(internal, "heartbeatIntervalMs", 30 * 60 * 1000);
    if (interval_ms <= 0) interval_ms = 30 * 60 * 1000;
    MIMI_LOGD(TAG, "Heartbeat service initialized (file: %s, interval: %ds)",
             heartbeat_file,
             interval_ms / 1000);
    return MIMI_OK;
}

mimi_err_t heartbeat_start(void)
{
    if (s_timer_handle != NULL) {
        MIMI_LOGW(TAG, "Heartbeat timer already running");
        return MIMI_OK;
    }

    mimi_cfg_obj_t internal = mimi_cfg_section("internal");
    int interval_ms = mimi_cfg_get_int(internal, "heartbeatIntervalMs", 30 * 60 * 1000);
    if (interval_ms <= 0) interval_ms = 30 * 60 * 1000;

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
