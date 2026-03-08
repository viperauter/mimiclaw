#include "runtime.h"
#include "os/os.h"
#include "config.h"
#include "log.h"

#include "mongoose.h"
#include <stdbool.h>

/* Global event loop for POSIX implementation (backed by Mongoose). */
static struct mg_mgr s_mgr;

/* Global flag to signal application shutdown */
static volatile bool s_should_exit = false;

mimi_err_t mimi_runtime_init(void)
{
    /* Set Mongoose log level based on current log level */
    if (!mimi_log_is_enabled()) {
        mg_log_level = MG_LL_NONE;
    } else {
        mimi_log_level_t current_level = mimi_log_get_level();
        switch (current_level) {
            case MIMI_LOG_ERROR:
                mg_log_level = MG_LL_ERROR;
                break;
            case MIMI_LOG_WARN:
                mg_log_level = MG_LL_INFO; // Mongoose doesn't have WARN level, use INFO instead
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
    mg_log_set(mg_log_level);

    /* Initialize global Mongoose manager used as the event loop. */
    mg_mgr_init(&s_mgr);

    /* Timers are implemented on top of the same event loop. */
    mimi_timer_set_event_loop(&s_mgr);

    return MIMI_OK;
}

mimi_err_t mimi_runtime_run(void)
{
    /* Main event loop: poll Mongoose manager until exit requested. */
    while (!s_should_exit) {
        mg_mgr_poll(&s_mgr, 100);
        
        /* Poll STDIO gateway for CLI input */
        extern void stdio_gateway_poll(void);
        stdio_gateway_poll();
    }

    MIMI_LOGI("runtime", "Exit requested, shutting down...");
    
    /* Cleanup */
    mg_mgr_free(&s_mgr);
    return MIMI_OK;
}

void mimi_runtime_request_exit(void)
{
    s_should_exit = true;
}

bool mimi_runtime_should_exit(void)
{
    return s_should_exit;
}

void *mimi_runtime_get_event_loop(void)
{
    return &s_mgr;
}

