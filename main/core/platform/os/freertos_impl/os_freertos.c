#include "../os.h"
#include "log.h"

/* FreeRTOS-based OS backend implementation.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

struct mimi_mutex {
    SemaphoreHandle_t sem;
};

struct mimi_cond {
    SemaphoreHandle_t sem;
    UBaseType_t count;
};

/* Opaque task handle: joinable task only. */
struct mimi_task_handle_inner {
    TaskHandle_t th;
    bool joined;
};

/* Opaque timer handle. */
struct mimi_timer_handle_inner {
    TimerHandle_t t;
};

/* Task trampoline structure */
typedef struct {
    mimi_task_fn_t fn;
    void *arg;
    char name[32];
} task_start_t;

static void task_trampoline(void *p)
{
    task_start_t *t = (task_start_t *)p;
    mimi_task_fn_t fn = t->fn;
    void *arg = t->arg;
    char name[32];
    strncpy(name, t->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    free(t);

    MIMI_LOGI("os", "task start: %s", name[0] ? name : "(unnamed)");
    fn(arg);
    MIMI_LOGI("os", "task exit: %s", name[0] ? name : "(unnamed)");
    vTaskDelete(NULL);
}

mimi_err_t mimi_task_create(const char *name, mimi_task_fn_t fn, void *arg,
                            uint32_t stack_size, int prio,
                            mimi_task_handle_t *out_handle)
{
    if (!fn) return MIMI_ERR_INVALID_ARG;

    MIMI_LOGI("os", "Creating task: %s (stack=%u, prio=%d)", name ? name : "unnamed", stack_size, prio);

    bool detached = (out_handle == NULL);
    task_start_t *t = (task_start_t *)calloc(1, sizeof(task_start_t));
    if (!t) {
        MIMI_LOGE("os", "Failed to allocate task_start_t");
        return MIMI_ERR_NO_MEM;
    }
    t->fn = fn;
    t->arg = arg;
    if (name) {
        strncpy(t->name, name, sizeof(t->name) - 1);
    }

    if (stack_size == 0) {
        stack_size = configMINIMAL_STACK_SIZE * 8; // Increase default stack size for mimiclaw tasks
        MIMI_LOGI("os", "Using default stack size: %u", stack_size);
    }

    if ((unsigned long)prio < tskIDLE_PRIORITY) {
        prio = tskIDLE_PRIORITY + 1;
        MIMI_LOGI("os", "Using default priority: %d", prio);
    }

    if (detached) {
        TaskHandle_t th;
        BaseType_t rc = xTaskCreate(task_trampoline, name ? name : "unnamed", 
                                   stack_size, t, prio, &th);
        if (rc != pdPASS) {
            MIMI_LOGE("os", "Failed to create detached task: %ld", (long)rc);
            free(t);
            return MIMI_ERR_FAIL;
        }
        MIMI_LOGI("os", "Created detached task: %s", name ? name : "unnamed");
        return MIMI_OK;
    }

    struct mimi_task_handle_inner *h = (struct mimi_task_handle_inner *)calloc(1, sizeof(*h));
    if (!h) {
        MIMI_LOGE("os", "Failed to allocate task_handle_inner");
        free(t);
        return MIMI_ERR_NO_MEM;
    }
    BaseType_t rc = xTaskCreate(task_trampoline, name ? name : "unnamed", 
                               stack_size, t, prio, &h->th);
    if (rc != pdPASS) {
        MIMI_LOGE("os", "Failed to create task: %ld", (long)rc);
        free(h);
        free(t);
        return MIMI_ERR_FAIL;
    }
    h->joined = false;
    *out_handle = (mimi_task_handle_t)h;
    MIMI_LOGI("os", "Created task: %s (handle=%p)", name ? name : "unnamed", h);
    return MIMI_OK;
}

mimi_err_t mimi_task_delete(mimi_task_handle_t handle)
{
    if (!handle) return MIMI_OK;
    struct mimi_task_handle_inner *h = (struct mimi_task_handle_inner *)handle;
    if (!h->joined) {
        vTaskDelete(h->th);
        h->joined = true;
    }
    free(h);
    return MIMI_OK;
}

mimi_err_t mimi_task_create_detached(const char *name, mimi_task_fn_t fn, void *arg)
{
    return mimi_task_create(name, fn, arg, 0, 0, NULL);
}

/* Timer callback trampoline */
static void timer_trampoline(TimerHandle_t xTimer)
{
    void *ctx = pvTimerGetTimerID(xTimer);
    mimi_timer_fn_t cb = (mimi_timer_fn_t)ctx;
    cb(NULL);
}

mimi_err_t mimi_timer_start(uint32_t period_ms, bool periodic,
                            mimi_timer_fn_t cb, void *ctx,
                            mimi_timer_handle_t *out_handle)
{
    if (!out_handle) return MIMI_ERR_INVALID_ARG;
    *out_handle = NULL;
    if (!cb) return MIMI_ERR_INVALID_ARG;

    struct mimi_timer_handle_inner *h = (struct mimi_timer_handle_inner *)calloc(1, sizeof(*h));
    if (!h) return MIMI_ERR_NO_MEM;

    UBaseType_t autoReload = periodic ? pdTRUE : pdFALSE;
    h->t = xTimerCreate("mimi_timer", pdMS_TO_TICKS(period_ms), 
                       autoReload, (void *)cb, timer_trampoline);
    if (!h->t) {
        free(h);
        return MIMI_ERR_NO_MEM;
    }

    if (xTimerStart(h->t, 0) != pdPASS) {
        xTimerDelete(h->t, 0);
        free(h);
        return MIMI_ERR_FAIL;
    }

    *out_handle = (mimi_timer_handle_t)h;
    return MIMI_OK;
}

void mimi_timer_stop(mimi_timer_handle_t *handle)
{
    if (!handle || !*handle) return;
    struct mimi_timer_handle_inner *h = (struct mimi_timer_handle_inner *)*handle;
    xTimerDelete(h->t, 0);
    free(h);
    *handle = NULL;
}

/* Not used in FreeRTOS implementation */
void mimi_timer_set_event_loop(void *mgr)
{
    (void)mgr;
    /* FreeRTOS timers don't require an external event loop */
}

mimi_err_t mimi_mutex_create(mimi_mutex_t **out)
{
    if (!out) return MIMI_ERR_INVALID_ARG;
    mimi_mutex_t *m = (mimi_mutex_t *)calloc(1, sizeof(*m));
    if (!m) return MIMI_ERR_NO_MEM;
    m->sem = xSemaphoreCreateMutex();
    if (!m->sem) {
        free(m);
        return MIMI_ERR_FAIL;
    }
    *out = m;
    return MIMI_OK;
}

void mimi_mutex_destroy(mimi_mutex_t *m)
{
    if (!m) return;
    vSemaphoreDelete(m->sem);
    free(m);
}

mimi_err_t mimi_mutex_lock(mimi_mutex_t *m)
{
    if (!m) return MIMI_ERR_INVALID_ARG;
    return (xSemaphoreTake(m->sem, portMAX_DELAY) == pdTRUE) ? MIMI_OK : MIMI_ERR_FAIL;
}

mimi_err_t mimi_mutex_unlock(mimi_mutex_t *m)
{
    if (!m) return MIMI_ERR_INVALID_ARG;
    return (xSemaphoreGive(m->sem) == pdTRUE) ? MIMI_OK : MIMI_ERR_FAIL;
}

mimi_err_t mimi_cond_create(mimi_cond_t **out)
{
    if (!out) return MIMI_ERR_INVALID_ARG;
    mimi_cond_t *c = (mimi_cond_t *)calloc(1, sizeof(*c));
    if (!c) return MIMI_ERR_NO_MEM;
    c->sem = xSemaphoreCreateCounting(100, 0); // Arbitrary max count
    if (!c->sem) {
        free(c);
        return MIMI_ERR_FAIL;
    }
    c->count = 0;
    *out = c;
    return MIMI_OK;
}

void mimi_cond_destroy(mimi_cond_t *c)
{
    if (!c) return;
    vSemaphoreDelete(c->sem);
    free(c);
}

mimi_err_t mimi_cond_wait(mimi_cond_t *c, mimi_mutex_t *m, uint32_t timeout_ms)
{
    if (!c || !m) return MIMI_ERR_INVALID_ARG;

    // Unlock the mutex first
    mimi_mutex_unlock(m);

    // Wait for the semaphore
    BaseType_t result;
    if (timeout_ms == UINT32_MAX) {
        result = xSemaphoreTake(c->sem, portMAX_DELAY);
    } else {
        result = xSemaphoreTake(c->sem, pdMS_TO_TICKS(timeout_ms));
    }

    // Lock the mutex again
    mimi_mutex_lock(m);

    if (result == pdTRUE) {
        c->count--;
        return MIMI_OK;
    } else {
        return MIMI_ERR_TIMEOUT;
    }
}

mimi_err_t mimi_cond_signal(mimi_cond_t *c)
{
    if (!c) return MIMI_ERR_INVALID_ARG;
    if (c->count < 100) { // Respect max count
        c->count++;
        if (xSemaphoreGive(c->sem) == pdTRUE) {
            return MIMI_OK;
        }
    }
    return MIMI_ERR_FAIL;
}

mimi_err_t mimi_cond_broadcast(mimi_cond_t *c)
{
    if (!c) return MIMI_ERR_INVALID_ARG;
    // For simplicity, just signal once (FreeRTOS doesn't有 native broadcast)
    return mimi_cond_signal(c);
}

/* -------------------------------------------------------------------------
 * OS backend init + time functions
 * ------------------------------------------------------------------------- */

mimi_err_t mimi_os_init(void)
{
    /* 对于主机上的 FreeRTOS POSIX 端口，不在这里启动调度器，
     * 由平台的 main() 负责调用 vTaskStartScheduler()。
     * 这里仅做一次简单的日志，便于确认后端类型。 */
    MIMI_LOGI("os", "Initializing FreeRTOS backend (no scheduler thread)");
    return MIMI_OK;
}

const char *mimi_os_get_version(void)
{
    return "FreeRTOS backend";
}

uint64_t mimi_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
}

void mimi_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* -------------------------------------------------------------------------
 * OS startup function
 * ------------------------------------------------------------------------- */

typedef struct {
    mimi_task_fn_t fn;
    void *arg;
} scheduler_task_ctx_t;

static scheduler_task_ctx_t s_scheduler_ctx;

static void scheduler_task_wrapper(void *pvParameters)
{
    (void)pvParameters;
    s_scheduler_ctx.fn(s_scheduler_ctx.arg);
    vTaskDelete(NULL);
    /* Application finished, exit the program */
    exit(0);
}

mimi_err_t mimi_os_start_scheduler(mimi_task_fn_t fn, void *arg)
{
    if (!fn) return MIMI_ERR_INVALID_ARG;
    
    s_scheduler_ctx.fn = fn;
    s_scheduler_ctx.arg = arg;
    
    if (xTaskCreate(scheduler_task_wrapper,
                    "main_task",
                    configMINIMAL_STACK_SIZE * 8,
                    NULL,
                    tskIDLE_PRIORITY + 2,
                    NULL) != pdPASS) {
        return MIMI_ERR_FAIL;
    }
    
    vTaskStartScheduler();
    
    /* Should never reach here */
    return MIMI_ERR_FAIL;
}

