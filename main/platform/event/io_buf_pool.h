#pragma once

#include "io_buf.h"
#include "mimi_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct io_buf_pool io_buf_pool_t;

/**
 * Create a fixed-size chunk pool.
 *
 * - chunk_size: capacity of each buffer returned from the pool
 * - chunk_count: maximum number of chunks managed by this pool
 * - a: optional allocator; if NULL, uses the global io allocator (io_set_allocator)
 */
io_buf_pool_t *io_buf_pool_create(size_t chunk_size, size_t chunk_count, const io_allocator_t *a);

void io_buf_pool_destroy(io_buf_pool_t *p);

/**
 * Allocate a chunk from the pool.
 *
 * Returns io_buf with:
 * - refcount = 1
 * - len = 0
 * - capacity = chunk_size
 *
 * When its refcount reaches 0, it is returned to the pool (not freed).
 */
io_buf_t *io_buf_pool_alloc(io_buf_pool_t *p);

size_t io_buf_pool_chunk_size(const io_buf_pool_t *p);
size_t io_buf_pool_capacity(const io_buf_pool_t *p); /* number of chunks */
size_t io_buf_pool_available(const io_buf_pool_t *p); /* approximate (thread-safe) */

#ifdef __cplusplus
}
#endif

