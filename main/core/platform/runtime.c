#include "runtime.h"
#include "os/os.h"
#include "config.h"
#include "config_view.h"
#include "log.h"
#include "event/event_bus.h"
#include "event/event_dispatcher.h"
#include "mongoose.h"

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "runtime";

/* Mongoose event loop manager */
static struct mg_mgr s_mgr;
/* Optional custom DNS URL, configured via env */
static char s_dns_url[128];

/* Runtime state */
static volatile mimi_runtime_state_t s_state = RUNTIME_STATE_IDLE;
static volatile bool s_should_exit = false;

/* Thread handle for the runtime thread */
static mimi_task_handle_t s_runtime_thread = NULL;

/* Mutex for state synchronization */
static mimi_mutex_t *s_state_mutex = NULL;

/* Event dispatcher */
static event_dispatcher_t *s_dispatcher = NULL;

/* Event loop thread function */
static void runtime_thread_fn(void *arg)
{
    (void)arg;

    MIMI_LOGI(TAG, "Runtime thread started");

    /* Main event loop */
    while (!s_should_exit) {
        mg_mgr_poll(&s_mgr, 10);
        
        /* Process send queue - execute pending sends in event loop thread */
        if (s_dispatcher) {
            event_dispatcher_drain_send(s_dispatcher);
        }
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
    mg_log_set(MG_LL_NONE);

    /* Initialize Mongoose manager */
    mg_mgr_init(&s_mgr);

    /* Optionally override DNS server from environment.
     * Example: export MIMI_DNS_SERVER=1.1.1.1 */
    const char *dns = getenv("MIMI_DNS_SERVER");
    if (dns && dns[0]) {
        snprintf(s_dns_url, sizeof(s_dns_url), "udp://%s:53", dns);
        s_mgr.dns4.url = s_dns_url;
        MIMI_LOGD(TAG, "Runtime DNS server set to %s (from environment)", s_dns_url);
    } else {
        /* Use DNS server from config */
        mimi_cfg_obj_t network = mimi_cfg_section("network");
        const char *dns_cfg = mimi_cfg_get_str(network, "dnsServer", "");
        if (dns_cfg && dns_cfg[0]) {
            snprintf(s_dns_url, sizeof(s_dns_url), "udp://%s:53", dns_cfg);
            s_mgr.dns4.url = s_dns_url;
            MIMI_LOGD(TAG, "Runtime DNS server set to %s (from config)", s_dns_url);
        } else {
            MIMI_LOGD(TAG, "Using default DNS server");
        }
    }

    /* Set timer event loop */
    mimi_timer_set_event_loop(&s_mgr);

    /* Create event bus */
    event_bus_t *event_bus = event_bus_create(64);
    if (!event_bus) {
        MIMI_LOGE(TAG, "Failed to create event bus");
        mg_mgr_free(&s_mgr);
        return MIMI_ERR_FAIL;
    }
    
    /* Set global event bus */
    event_bus_set_global(event_bus);

    /* Create event dispatcher with 2 worker threads */
    s_dispatcher = event_dispatcher_create(2, event_bus);
    if (!s_dispatcher) {
        MIMI_LOGE(TAG, "Failed to create event dispatcher");
        event_bus_destroy(event_bus);
        mg_mgr_free(&s_mgr);
        return MIMI_ERR_FAIL;
    }
    
    event_dispatcher_set_global(s_dispatcher);

    s_state = RUNTIME_STATE_IDLE;
    s_should_exit = false;
    s_runtime_thread = NULL;

    MIMI_LOGD(TAG, "Runtime initialized");
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

    /* Start event dispatcher workers */
    mimi_err_t err = event_dispatcher_start(s_dispatcher);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to start event dispatcher");
        mimi_mutex_lock(s_state_mutex);
        s_state = RUNTIME_STATE_IDLE;
        mimi_mutex_unlock(s_state_mutex);
        return err;
    }

    /* Create runtime thread */
    err = mimi_task_create("runtime", runtime_thread_fn, NULL, 0, 0, &s_runtime_thread);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create runtime thread");
        event_dispatcher_stop(s_dispatcher);
        mimi_mutex_lock(s_state_mutex);
        s_state = RUNTIME_STATE_IDLE;
        mimi_mutex_unlock(s_state_mutex);
        return err;
    }

    MIMI_LOGD(TAG, "Runtime started");
    return MIMI_OK;
}

void mimi_runtime_stop(void)
{
    mimi_mutex_lock(s_state_mutex);

    if (s_state != RUNTIME_STATE_RUNNING) {
        /* Runtime may have stopped itself; still join/free the thread handle if needed. */
        mimi_task_handle_t th = s_runtime_thread;
        s_runtime_thread = NULL;
        mimi_mutex_unlock(s_state_mutex);
        if (th != NULL) {
            (void)mimi_task_delete(th);
        }
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

    /* Stop event dispatcher */
    if (s_dispatcher) {
        event_dispatcher_stop(s_dispatcher);
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

    /* Cleanup Mongoose (connections, internal resources). */
    mg_mgr_free(&s_mgr);

    /* Cleanup event dispatcher */
    if (s_dispatcher) {
        event_dispatcher_destroy(s_dispatcher);
        s_dispatcher = NULL;
        event_dispatcher_set_global(NULL);
    }

    /* Cleanup event bus */
    event_bus_t *event_bus = event_bus_get_global();
    if (event_bus) {
        event_bus_destroy(event_bus);
        event_bus_set_global(NULL);
    }

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

void *mimi_runtime_get_dispatcher(void)
{
    return s_dispatcher;
}
