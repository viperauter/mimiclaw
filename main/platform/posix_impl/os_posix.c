#include "platform/os.h"
#include "platform/log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

struct mimi_mutex {
    pthread_mutex_t m;
};

struct mimi_cond {
    pthread_cond_t c;
};

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

mimi_err_t mimi_task_create_detached(const char *name, mimi_task_fn_t fn, void *arg)
{
    if (!fn) return MIMI_ERR_INVALID_ARG;

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
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

    int rc = pthread_create(&th, &attr, task_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return MIMI_ERR_FAIL;
    }
    return MIMI_OK;
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
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_ms * 1000000ULL;
    out->tv_sec = ts.tv_sec + (time_t)(nsec / 1000000000ULL);
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

