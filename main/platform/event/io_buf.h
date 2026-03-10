#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct io_buf io_buf_t;

struct io_buf {
    uint8_t *base;          /* Pointer to data (named 'base' for libuv compatibility) */
    size_t len;             /* Data length */
    
    /* Reference counting for zero-copy */
    volatile int refcount;
    
    /* Capacity for allocated buffers */
    size_t capacity;
};

/**
 * Allocate a new io_buf with data buffer.
 * Returns buffer with refcount = 1.
 */
io_buf_t *io_buf_alloc(size_t capacity);

/**
 * Create io_buf from existing data (takes ownership).
 * Returns buffer with refcount = 1.
 */
io_buf_t *io_buf_from_data(void *data, size_t len);

/**
 * Create io_buf from const data (copies data).
 * Returns buffer with refcount = 1.
 */
io_buf_t *io_buf_from_const(const void *data, size_t len);

/**
 * Increment reference count.
 * Returns the same pointer for convenience.
 */
io_buf_t *io_buf_ref(io_buf_t *buf);

/**
 * Decrement reference count.
 * Frees buffer when refcount reaches 0.
 */
void io_buf_unref(io_buf_t *buf);

/**
 * Get current reference count.
 */
int io_buf_get_refcount(io_buf_t *buf);

#ifdef __cplusplus
}
#endif
