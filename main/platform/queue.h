#pragma once

#include <stddef.h>
#include <stdint.h>
#include "platform/mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mimi_queue mimi_queue_t;

mimi_err_t mimi_queue_create(mimi_queue_t **out, size_t elem_size, size_t capacity);
void mimi_queue_destroy(mimi_queue_t *q);

/* Sends a copy of elem bytes into queue. */
mimi_err_t mimi_queue_send(mimi_queue_t *q, const void *elem, uint32_t timeout_ms);

/* Receives one element into elem_out (must have elem_size). */
mimi_err_t mimi_queue_recv(mimi_queue_t *q, void *elem_out, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

