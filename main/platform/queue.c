#include "queue.h"
#include "os/os.h"
#include <stdlib.h>
#include <string.h>

struct mimi_queue {
    size_t elem_size;
    size_t cap;
    size_t head;
    size_t tail;
    size_t count;
    uint8_t *buf;
    mimi_mutex_t *mu;
    mimi_cond_t *cv_not_empty;
    mimi_cond_t *cv_not_full;
};

mimi_err_t mimi_queue_create(mimi_queue_t **out, size_t elem_size, size_t capacity)
{
    if (!out || elem_size == 0 || capacity == 0) return MIMI_ERR_INVALID_ARG;

    mimi_queue_t *q = (mimi_queue_t *)calloc(1, sizeof(*q));
    if (!q) return MIMI_ERR_NO_MEM;

    q->buf = (uint8_t *)calloc(capacity, elem_size);
    if (!q->buf) {
        free(q);
        return MIMI_ERR_NO_MEM;
    }

    q->elem_size = elem_size;
    q->cap = capacity;

    if (mimi_mutex_create(&q->mu) != MIMI_OK ||
        mimi_cond_create(&q->cv_not_empty) != MIMI_OK ||
        mimi_cond_create(&q->cv_not_full) != MIMI_OK) {
        mimi_queue_destroy(q);
        return MIMI_ERR_FAIL;
    }

    *out = q;
    return MIMI_OK;
}

void mimi_queue_destroy(mimi_queue_t *q)
{
    if (!q) return;
    if (q->cv_not_empty) mimi_cond_destroy(q->cv_not_empty);
    if (q->cv_not_full) mimi_cond_destroy(q->cv_not_full);
    if (q->mu) mimi_mutex_destroy(q->mu);
    free(q->buf);
    free(q);
}

mimi_err_t mimi_queue_send(mimi_queue_t *q, const void *elem, uint32_t timeout_ms)
{
    if (!q || !elem) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(q->mu);
    while (q->count == q->cap) {
        mimi_err_t w = mimi_cond_wait(q->cv_not_full, q->mu, timeout_ms);
        if (w != MIMI_OK) {
            mimi_mutex_unlock(q->mu);
            return w;
        }
    }

    uint8_t *dst = q->buf + (q->tail * q->elem_size);
    memcpy(dst, elem, q->elem_size);
    q->tail = (q->tail + 1) % q->cap;
    q->count++;

    mimi_cond_signal(q->cv_not_empty);
    mimi_mutex_unlock(q->mu);
    return MIMI_OK;
}

mimi_err_t mimi_queue_recv(mimi_queue_t *q, void *elem_out, uint32_t timeout_ms)
{
    if (!q || !elem_out) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(q->mu);
    while (q->count == 0) {
        mimi_err_t w = mimi_cond_wait(q->cv_not_empty, q->mu, timeout_ms);
        if (w != MIMI_OK) {
            mimi_mutex_unlock(q->mu);
            return w;
        }
    }

    uint8_t *src = q->buf + (q->head * q->elem_size);
    memcpy(elem_out, src, q->elem_size);
    q->head = (q->head + 1) % q->cap;
    q->count--;

    mimi_cond_signal(q->cv_not_full);
    mimi_mutex_unlock(q->mu);
    return MIMI_OK;
}

mimi_err_t mimi_queue_try_send(mimi_queue_t *q, const void *elem)
{
    if (!q || !elem) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(q->mu);
    if (q->count == q->cap) {
        mimi_mutex_unlock(q->mu);
        return MIMI_ERR_WOULD_BLOCK;
    }

    uint8_t *dst = q->buf + (q->tail * q->elem_size);
    memcpy(dst, elem, q->elem_size);
    q->tail = (q->tail + 1) % q->cap;
    q->count++;

    mimi_cond_signal(q->cv_not_empty);
    mimi_mutex_unlock(q->mu);
    return MIMI_OK;
}

mimi_err_t mimi_queue_try_recv(mimi_queue_t *q, void *elem_out)
{
    if (!q || !elem_out) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(q->mu);
    if (q->count == 0) {
        mimi_mutex_unlock(q->mu);
        return MIMI_ERR_WOULD_BLOCK;
    }

    uint8_t *src = q->buf + (q->head * q->elem_size);
    memcpy(elem_out, src, q->elem_size);
    q->head = (q->head + 1) % q->cap;
    q->count--;

    mimi_cond_signal(q->cv_not_full);
    mimi_mutex_unlock(q->mu);
    return MIMI_OK;
}

size_t mimi_queue_count(mimi_queue_t *q)
{
    if (!q) return 0;
    mimi_mutex_lock(q->mu);
    size_t count = q->count;
    mimi_mutex_unlock(q->mu);
    return count;
}

