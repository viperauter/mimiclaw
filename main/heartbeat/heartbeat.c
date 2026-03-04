#include "heartbeat/heartbeat.h"
#include "mimi_config.h"
#include "bus/message_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "platform/log.h"
#include "mongoose.h"

static const char *TAG = "heartbeat";

#define HEARTBEAT_PROMPT \
    "Read " MIMI_HEARTBEAT_FILE " and follow any instructions or tasks listed there. " \
    "If nothing needs attention, reply with just: HEARTBEAT_OK"

static struct mg_mgr *s_mgr = NULL;
static struct mg_timer *s_timer = NULL;

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
    FILE *f = fopen(MIMI_HEARTBEAT_FILE, "r");
    if (!f) {
        return false;
    }

    char line[256];
    bool found_task = false;

    while (fgets(line, sizeof(line), f)) {
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

    fclose(f);
    return found_task;
}

/* ── Send heartbeat to agent ──────────────────────────────────── */

static bool heartbeat_send(void)
{
    if (!heartbeat_has_tasks()) {
        MIMI_LOGD(TAG, "No actionable tasks in HEARTBEAT.md");
        return false;
    }

    mimi_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, MIMI_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "heartbeat", sizeof(msg.chat_id) - 1);
    msg.content = strdup(HEARTBEAT_PROMPT);

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
    MIMI_LOGI(TAG, "Heartbeat service initialized (file: %s, interval: %ds)",
             MIMI_HEARTBEAT_FILE, MIMI_HEARTBEAT_INTERVAL_MS / 1000);
    return MIMI_OK;
}

mimi_err_t heartbeat_start(void)
{
    if (!s_mgr) {
        MIMI_LOGW(TAG, "Heartbeat start requires mg_mgr (call heartbeat_set_mgr first)");
        return MIMI_ERR_INVALID_STATE;
    }
    if (s_timer) {
        MIMI_LOGW(TAG, "Heartbeat timer already running");
        return MIMI_OK;
    }

    s_timer = mg_timer_add(s_mgr, MIMI_HEARTBEAT_INTERVAL_MS, MG_TIMER_REPEAT, heartbeat_timer_cb, NULL);
    if (!s_timer) return MIMI_ERR_NO_MEM;
    MIMI_LOGI(TAG, "Heartbeat started (every %d min)", MIMI_HEARTBEAT_INTERVAL_MS / 60000);
    return MIMI_OK;
}

void heartbeat_stop(void)
{
    if (s_mgr && s_timer) {
        mg_timer_free(&s_mgr->timers, s_timer);
        s_timer = NULL;
        MIMI_LOGI(TAG, "Heartbeat stopped");
    }
}

bool heartbeat_trigger(void)
{
    return heartbeat_send();
}

void heartbeat_set_mgr(struct mg_mgr *mgr)
{
    s_mgr = mgr;
}
