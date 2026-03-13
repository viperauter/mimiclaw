#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct io_buf io_buf_t;

typedef void *(*io_alloc_fn)(size_t size, void *ctx);
typedef void (*io_free_fn)(void *ptr, void *ctx);

typedef struct {
    io_alloc_fn alloc;
    io_free_fn free;
    void *ctx;
} io_allocator_t;

/**
 * Set global allocator used by io_buf default implementation.
 *
 * - If not set, defaults to malloc/free.
 * - Intended for RTOS targets that want to route allocations to a specific heap.
 *
 * Note: should be called during system init (before heavy concurrent use).
 */
void io_set_allocator(const io_allocator_t *a);
void io_get_allocator(io_allocator_t *out);

typedef void (*io_buf_release_fn)(io_buf_t *buf, void *owner);

struct io_buf {
    uint8_t *base;          /* Pointer to data (named 'base' for libuv compatibility) */
    size_t len;             /* Data length */
    
    /* Reference counting for zero-copy */
    volatile int refcount;
    
    /* Capacity for allocated buffers */
    size_t capacity;

    /* Custom release hook (optional).
     * If set, io_buf_unref() calls release(buf, owner) when refcount reaches 0. */
    io_buf_release_fn release;
    void *owner;
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
