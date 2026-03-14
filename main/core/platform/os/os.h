#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Task (opaque handle; no pthread/FreeRTOS types exposed)
 * ------------------------------------------------------------------------- */
typedef struct mimi_task_handle_inner *mimi_task_handle_t;
typedef void (*mimi_task_fn_t)(void *arg);

/**
 * Create a task. If out_handle is NULL, creates a detached task (no handle,
 * cannot be deleted). If out_handle is non-NULL, returns a handle for
 * mimi_task_delete. stack_size/prio: on ESP32 used; on POSIX ignored (0 = default).
 */
mimi_err_t mimi_task_create(const char *name, mimi_task_fn_t fn, void *arg,
                            uint32_t stack_size, int prio,
                            mimi_task_handle_t *out_handle);

/** Delete (and wait for) a non-detached task. No-op for NULL/detached. */
mimi_err_t mimi_task_delete(mimi_task_handle_t handle);

/** Convenience: create detached task (same as mimi_task_create(..., NULL)). */
mimi_err_t mimi_task_create_detached(const char *name, mimi_task_fn_t fn, void *arg);

/* -------------------------------------------------------------------------
 * Timer (opaque handle)
 * ------------------------------------------------------------------------- */
typedef struct mimi_timer_handle_inner *mimi_timer_handle_t;
typedef void (*mimi_timer_fn_t)(void *ctx);

mimi_err_t mimi_timer_start(uint32_t period_ms, bool periodic,
                            mimi_timer_fn_t cb, void *ctx,
                            mimi_timer_handle_t *out_handle);
void mimi_timer_stop(mimi_timer_handle_t *handle);

/**
 * Optional: inject event loop for timer driver (e.g. POSIX uses mg_mgr).
 * Call before any mimi_timer_start when using Mongoose-backed impl.
 */
void mimi_timer_set_event_loop(void *mgr);

/* -------------------------------------------------------------------------
 * Mutex + cond (for platform impls e.g. queue/kv; not for app code)
 * ------------------------------------------------------------------------- */
typedef struct mimi_mutex mimi_mutex_t;
typedef struct mimi_cond mimi_cond_t;

mimi_err_t mimi_mutex_create(mimi_mutex_t **out);
void mimi_mutex_destroy(mimi_mutex_t *m);
mimi_err_t mimi_mutex_lock(mimi_mutex_t *m);
mimi_err_t mimi_mutex_unlock(mimi_mutex_t *m);

mimi_err_t mimi_cond_create(mimi_cond_t **out);
void mimi_cond_destroy(mimi_cond_t *c);
mimi_err_t mimi_cond_wait(mimi_cond_t *c, mimi_mutex_t *m, uint32_t timeout_ms);
mimi_err_t mimi_cond_signal(mimi_cond_t *c);
mimi_err_t mimi_cond_broadcast(mimi_cond_t *c);

/**
 * Initialize OS backend.
 * 
 * @return MIMI_OK on success, error code on failure.
 */
mimi_err_t mimi_os_init(void);

/**
 * Get OS backend version information.
 *
 * @return A string containing the OS backend name and version.
 */
const char *mimi_os_get_version(void);

/**
 * Start OS scheduler and run the given function in a task context.
 * 
 * For POSIX backends, this directly calls the function.
 * For FreeRTOS backends, this creates a task, starts the scheduler,
 * and the function runs in the task context.
 * 
 * @param fn Function to run (must match mimi_task_fn_t signature)
 * @param arg Argument to pass to the function
 * @return MIMI_OK on success, error code on failure.
 */
mimi_err_t mimi_os_start_scheduler(mimi_task_fn_t fn, void *arg);

/* -------------------------------------------------------------------------
 * Time functions
 * ------------------------------------------------------------------------- */
uint64_t mimi_time_ms(void);
void mimi_sleep_ms(uint32_t ms);

#ifdef __cplusplus
};
#endif
