#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "platform/mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mimi_task mimi_task_t;
typedef struct mimi_mutex mimi_mutex_t;
typedef struct mimi_cond mimi_cond_t;

typedef void (*mimi_task_fn_t)(void *arg);

/* Create a detached background task/thread. */
mimi_err_t mimi_task_create_detached(const char *name, mimi_task_fn_t fn, void *arg);

/* Mutex + condvar primitives (opaque; do not leak OS types). */
mimi_err_t mimi_mutex_create(mimi_mutex_t **out);
void mimi_mutex_destroy(mimi_mutex_t *m);
mimi_err_t mimi_mutex_lock(mimi_mutex_t *m);
mimi_err_t mimi_mutex_unlock(mimi_mutex_t *m);

mimi_err_t mimi_cond_create(mimi_cond_t **out);
void mimi_cond_destroy(mimi_cond_t *c);
mimi_err_t mimi_cond_wait(mimi_cond_t *c, mimi_mutex_t *m, uint32_t timeout_ms);
mimi_err_t mimi_cond_signal(mimi_cond_t *c);
mimi_err_t mimi_cond_broadcast(mimi_cond_t *c);

#ifdef __cplusplus
}
#endif

