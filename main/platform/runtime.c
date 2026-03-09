#include "runtime.h"
#include "os/os.h"
#include "config.h"
#include "log.h"
#include "mongoose.h"

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "runtime";

/* Mongoose event loop manager */
static struct mg_mgr s_mgr;
/* Optional custom DNS URL, configured via env */
static char s_dns_url[64];

/* Runtime state */
static volatile mimi_runtime_state_t s_state = RUNTIME_STATE_IDLE;
static volatile bool s_should_exit = false;

/* Thread handle for the runtime thread */
static mimi_task_handle_t s_runtime_thread = NULL;

/* Mutex for state synchronization */
static mimi_mutex_t *s_state_mutex = NULL;

/* Event loop thread function */
static void runtime_thread_fn(void *arg)
{
    (void)arg;

    MIMI_LOGI(TAG, "Runtime thread started");

    /* Main event loop */
    while (!s_should_exit) {
        mg_mgr_poll(&s_mgr, 100);
    }

    MIMI_LOGI(TAG, "Runtime thread exiting");

    /* Update state to STOPPED when thread exits */
    mimi_mutex_lock(s_state_mutex);
    s_state = RUNTIME_STATE_STOPPED;
    mimi_mutex_unlock(s_state_mutex);
}

mimi_err_t mimi_runtime_init(void)
{
    if (s_state != RUNTIME_STATE_IDLE && s_state != RUNTIME_STATE_STOPPED) {
        MIMI_LOGW(TAG, "Runtime already initialized");
        return MIMI_OK;
    }

    /* Create state mutex */
    if (s_state_mutex == NULL) {
        if (mimi_mutex_create(&s_state_mutex) != MIMI_OK) {
            MIMI_LOGE(TAG, "Failed to create state mutex");
            return MIMI_ERR_FAIL;
        }
    }

    /* Set Mongoose log level */
    if (!mimi_log_is_enabled()) {
        mg_log_level = MG_LL_NONE;
    } else {
        mimi_log_level_t current_level = mimi_log_get_level();
        switch (current_level) {
            case MIMI_LOG_ERROR:
                mg_log_level = MG_LL_ERROR;
                break;
            case MIMI_LOG_WARN:
                mg_log_level = MG_LL_INFO;
                break;
            case MIMI_LOG_INFO:
                mg_log_level = MG_LL_INFO;
                break;
            case MIMI_LOG_DEBUG:
                mg_log_level = MG_LL_DEBUG;
                break;
            default:
                mg_log_level = MG_LL_INFO;
                break;
        }
    }
    //mg_log_set(mg_log_level);

    /* Initialize Mongoose manager */
    mg_mgr_init(&s_mgr);

    /* Optionally override DNS server from environment.
     * Example: export MIMI_DNS_SERVER=1.1.1.1 */
    const char *dns = getenv("MIMI_DNS_SERVER");
    if (dns && dns[0]) {
        snprintf(s_dns_url, sizeof(s_dns_url), "udp://%s:53", dns);
        s_mgr.dns4.url = s_dns_url;
        MIMI_LOGI(TAG, "Runtime DNS server set to %s", s_dns_url);
    }

    /* Set timer event loop */
    mimi_timer_set_event_loop(&s_mgr);

    s_state = RUNTIME_STATE_IDLE;
    s_should_exit = false;
    s_runtime_thread = NULL;

    MIMI_LOGI(TAG, "Runtime initialized");
    return MIMI_OK;
}

mimi_err_t mimi_runtime_start(void)
{
    mimi_mutex_lock(s_state_mutex);

    if (s_state == RUNTIME_STATE_RUNNING) {
        MIMI_LOGW(TAG, "Runtime already running");
        mimi_mutex_unlock(s_state_mutex);
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_state != RUNTIME_STATE_IDLE && s_state != RUNTIME_STATE_STOPPED) {
        MIMI_LOGE(TAG, "Runtime in invalid state: %d", s_state);
        mimi_mutex_unlock(s_state_mutex);
        return MIMI_ERR_INVALID_STATE;
    }

    s_should_exit = false;
    s_state = RUNTIME_STATE_RUNNING;

    mimi_mutex_unlock(s_state_mutex);

    /* Create runtime thread */
    mimi_err_t err = mimi_task_create("runtime", runtime_thread_fn, NULL, 0, 0, &s_runtime_thread);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create runtime thread");
        mimi_mutex_lock(s_state_mutex);
        s_state = RUNTIME_STATE_IDLE;
        mimi_mutex_unlock(s_state_mutex);
        return err;
    }

    MIMI_LOGI(TAG, "Runtime started");
    return MIMI_OK;
}

void mimi_runtime_stop(void)
{
    mimi_mutex_lock(s_state_mutex);

    if (s_state != RUNTIME_STATE_RUNNING) {
        MIMI_LOGW(TAG, "Runtime not running");
        mimi_mutex_unlock(s_state_mutex);
        return;
    }

    s_state = RUNTIME_STATE_STOPPING;
    s_should_exit = true;

    mimi_mutex_unlock(s_state_mutex);

    MIMI_LOGI(TAG, "Stopping runtime...");

    /* Wait for thread to finish */
    if (s_runtime_thread != NULL) {
        mimi_task_delete(s_runtime_thread);
        s_runtime_thread = NULL;
    }

    mimi_mutex_lock(s_state_mutex);
    s_state = RUNTIME_STATE_STOPPED;
    mimi_mutex_unlock(s_state_mutex);

    MIMI_LOGI(TAG, "Runtime stopped");
}

void mimi_runtime_deinit(void)
{
    mimi_mutex_lock(s_state_mutex);

    if (s_state == RUNTIME_STATE_RUNNING) {
        mimi_mutex_unlock(s_state_mutex);
        mimi_runtime_stop();
        mimi_mutex_lock(s_state_mutex);
    }

    /* Cleanup Mongoose */
    mg_mgr_free(&s_mgr);

    /* Destroy mutex */
    if (s_state_mutex != NULL) {
        mimi_mutex_destroy(s_state_mutex);
        s_state_mutex = NULL;
    }

    s_state = RUNTIME_STATE_IDLE;
    s_should_exit = false;
    s_runtime_thread = NULL;

    mimi_mutex_unlock(s_state_mutex);

    MIMI_LOGI(TAG, "Runtime deinitialized");
}

mimi_runtime_state_t mimi_runtime_get_state(void)
{
    mimi_runtime_state_t state;
    mimi_mutex_lock(s_state_mutex);
    state = s_state;
    mimi_mutex_unlock(s_state_mutex);
    return state;
}

bool mimi_runtime_is_running(void)
{
    return mimi_runtime_get_state() == RUNTIME_STATE_RUNNING;
}

void mimi_runtime_request_exit(void)
{
    s_should_exit = true;
    MIMI_LOGI(TAG, "Exit requested");
}

bool mimi_runtime_should_exit(void)
{
    return s_should_exit;
}

void *mimi_runtime_get_event_loop(void)
{
    return &s_mgr;
}
