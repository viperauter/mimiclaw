#include "os/os.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include "mongoose.h"

struct mimi_mutex {
    pthread_mutex_t m;
};

struct mimi_cond {
    pthread_cond_t c;
};

/* Opaque task handle: joinable task only. */
struct mimi_task_handle_inner {
    pthread_t th;
};

/* Opaque timer handle: holds mg_timer + mg_mgr for mimi_timer_stop. */
struct mimi_timer_handle_inner {
    struct mg_mgr *mgr;
    struct mg_timer *t;
};

/* Event loop used by timers (owned by runtime_posix). */
static struct mg_mgr *s_timer_mgr;

typedef struct {
    mimi_task_fn_t fn;
    void *arg;
    char name[32];
} task_start_t;

static void *task_trampoline(void *p)
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
    return NULL;
}

mimi_err_t mimi_task_create(const char *name, mimi_task_fn_t fn, void *arg,
                            uint32_t stack_size, int prio,
                            mimi_task_handle_t *out_handle)
{
    (void)stack_size;
    (void)prio;
    if (!fn) return MIMI_ERR_INVALID_ARG;

    int detached = (out_handle == NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (detached)
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    task_start_t *t = (task_start_t *)calloc(1, sizeof(task_start_t));
    if (!t) {
        pthread_attr_destroy(&attr);
        return MIMI_ERR_NO_MEM;
    }
    t->fn = fn;
    t->arg = arg;
    if (name) {
        strncpy(t->name, name, sizeof(t->name) - 1);
    }

    if (detached) {
        pthread_t th;
        int rc = pthread_create(&th, &attr, task_trampoline, t);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            free(t);
            return MIMI_ERR_FAIL;
        }
        return MIMI_OK;
    }

    struct mimi_task_handle_inner *h = (struct mimi_task_handle_inner *)calloc(1, sizeof(*h));
    if (!h) {
        pthread_attr_destroy(&attr);
        free(t);
        return MIMI_ERR_NO_MEM;
    }
    int rc = pthread_create(&h->th, &attr, task_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(h);
        free(t);
        return MIMI_ERR_FAIL;
    }
    *out_handle = (mimi_task_handle_t)h;
    return MIMI_OK;
}

mimi_err_t mimi_task_delete(mimi_task_handle_t handle)
{
    if (!handle) return MIMI_OK;
    struct mimi_task_handle_inner *h = (struct mimi_task_handle_inner *)handle;
    int rc = pthread_join(h->th, NULL);
    free(h);
    return (rc == 0) ? MIMI_OK : MIMI_ERR_FAIL;
}

mimi_err_t mimi_task_create_detached(const char *name, mimi_task_fn_t fn, void *arg)
{
    return mimi_task_create(name, fn, arg, 0, 0, NULL);
}

/* -------------------------------------------------------------------------
 * Timer (POSIX: backed by global mg_mgr event loop)
 * ------------------------------------------------------------------------- */
void mimi_timer_set_event_loop(void *mgr)
{
    s_timer_mgr = (struct mg_mgr *)mgr;
}

mimi_err_t mimi_timer_start(uint32_t period_ms, bool periodic,
                            mimi_timer_fn_t cb, void *ctx,
                            mimi_timer_handle_t *out_handle)
{
    if (!out_handle) return MIMI_ERR_INVALID_ARG;
    *out_handle = NULL;
    if (!s_timer_mgr) return MIMI_ERR_INVALID_STATE;
    if (!cb) return MIMI_ERR_INVALID_ARG;

    struct mimi_timer_handle_inner *h = (struct mimi_timer_handle_inner *)calloc(1, sizeof(*h));
    if (!h) return MIMI_ERR_NO_MEM;
    h->mgr = s_timer_mgr;
    unsigned flags = periodic ? MG_TIMER_REPEAT : MG_TIMER_ONCE;
    h->t = mg_timer_add(s_timer_mgr, (uint64_t)period_ms, flags, (void (*)(void *))cb, ctx);
    if (!h->t) {
        free(h);
        return MIMI_ERR_NO_MEM;
    }
    *out_handle = (mimi_timer_handle_t)h;
    return MIMI_OK;
}

void mimi_timer_stop(mimi_timer_handle_t *handle)
{
    if (!handle || !*handle) return;
    struct mimi_timer_handle_inner *h = (struct mimi_timer_handle_inner *)*handle;
    mg_timer_free(&h->mgr->timers, h->t);
    free(h);
    *handle = NULL;
}

mimi_err_t mimi_mutex_create(mimi_mutex_t **out)
{
    if (!out) return MIMI_ERR_INVALID_ARG;
    mimi_mutex_t *m = (mimi_mutex_t *)calloc(1, sizeof(*m));
    if (!m) return MIMI_ERR_NO_MEM;
    if (pthread_mutex_init(&m->m, NULL) != 0) {
        free(m);
        return MIMI_ERR_FAIL;
    }
    *out = m;
    return MIMI_OK;
}

void mimi_mutex_destroy(mimi_mutex_t *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

mimi_err_t mimi_mutex_lock(mimi_mutex_t *m)
{
    if (!m) return MIMI_ERR_INVALID_ARG;
    return (pthread_mutex_lock(&m->m) == 0) ? MIMI_OK : MIMI_ERR_FAIL;
}

mimi_err_t mimi_mutex_unlock(mimi_mutex_t *m)
{
    if (!m) return MIMI_ERR_INVALID_ARG;
    return (pthread_mutex_unlock(&m->m) == 0) ? MIMI_OK : MIMI_ERR_FAIL;
}

mimi_err_t mimi_cond_create(mimi_cond_t **out)
{
    if (!out) return MIMI_ERR_INVALID_ARG;
    mimi_cond_t *c = (mimi_cond_t *)calloc(1, sizeof(*c));
    if (!c) return MIMI_ERR_NO_MEM;
    if (pthread_cond_init(&c->c, NULL) != 0) {
        free(c);
        return MIMI_ERR_FAIL;
    }
    *out = c;
    return MIMI_OK;
}

void mimi_cond_destroy(mimi_cond_t *c)
{
    if (!c) return;
    pthread_cond_destroy(&c->c);
    free(c);
}

static void abs_deadline_from_now(uint32_t timeout_ms, struct timespec *out)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t nsec = (uint64_t)tv.tv_usec * 1000ULL + (uint64_t)timeout_ms * 1000000ULL;
    out->tv_sec = tv.tv_sec + (time_t)(nsec / 1000000000ULL);
    out->tv_nsec = (long)(nsec % 1000000000ULL);
}

mimi_err_t mimi_cond_wait(mimi_cond_t *c, mimi_mutex_t *m, uint32_t timeout_ms)
{
    if (!c || !m) return MIMI_ERR_INVALID_ARG;

    if (timeout_ms == UINT32_MAX) {
        return (pthread_cond_wait(&c->c, &m->m) == 0) ? MIMI_OK : MIMI_ERR_FAIL;
    }

    struct timespec dl;
    abs_deadline_from_now(timeout_ms, &dl);
    int rc = pthread_cond_timedwait(&c->c, &m->m, &dl);
    if (rc == 0) return MIMI_OK;
    if (rc == ETIMEDOUT) return MIMI_ERR_TIMEOUT;
    return MIMI_ERR_FAIL;
}

mimi_err_t mimi_cond_signal(mimi_cond_t *c)
{
    if (!c) return MIMI_ERR_INVALID_ARG;
    return (pthread_cond_signal(&c->c) == 0) ? MIMI_OK : MIMI_ERR_FAIL;
}

mimi_err_t mimi_cond_broadcast(mimi_cond_t *c)
{
    if (!c) return MIMI_ERR_INVALID_ARG;
    return (pthread_cond_broadcast(&c->c) == 0) ? MIMI_OK : MIMI_ERR_FAIL;
}

mimi_err_t mimi_os_init(void)
{
    /* POSIX backend doesn't require initialization */
    return MIMI_OK;
}

const char *mimi_os_get_version(void)
{
    return "POSIX backend";
}

uint64_t mimi_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
}

void mimi_sleep_ms(uint32_t ms)
{
    usleep((useconds_t)ms * 1000U);
}

/* -------------------------------------------------------------------------
 * OS startup function
 * ------------------------------------------------------------------------- */

mimi_err_t mimi_os_start_scheduler(mimi_task_fn_t fn, void *arg)
{
    if (!fn) return MIMI_ERR_INVALID_ARG;
    fn(arg);
    return MIMI_OK;
}

