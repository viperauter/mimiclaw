#pragma once

#include "io_buf.h"
#include "io_buf_pool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct io_chain io_chain_t;

typedef struct {
    uint8_t *base;
    size_t len;
} io_span_t;

typedef struct {
    const void *p; /* internal */
} io_chain_iter_t;

/**
 * Create a chunked chain buffer.
 *
 * - If pool is provided, new chunks are allocated from the pool (fixed chunk size).
 * - If pool is NULL, chunks are allocated via io_buf_alloc() with a default chunk size.
 */
io_chain_t *io_chain_create(io_buf_pool_t *pool);

io_chain_t *io_chain_ref(io_chain_t *c);
void io_chain_unref(io_chain_t *c);

/** Total bytes across all chunks. */
size_t io_chain_len(const io_chain_t *c);

/** Number of chunks currently held. */
size_t io_chain_chunk_count(const io_chain_t *c);

/**
 * Append bytes to the chain, allocating new chunks as needed.
 *
 * Returns MIMI_OK on success.
 */
mimi_err_t io_chain_append_bytes(io_chain_t *c, const void *data, size_t len);

/**
 * Append an existing buffer as a whole chunk.
 * The chain takes a reference to buf.
 */
mimi_err_t io_chain_append_buf(io_chain_t *c, io_buf_t *buf);

/**
 * Export chain chunks into spans.
 *
 * Returns number of spans written to out (<= out_cap).
 */
size_t io_chain_to_spans(const io_chain_t *c, io_span_t *out, size_t out_cap);

/**
 * Initialize an iterator over non-empty chunks in the chain.
 */
void io_chain_iter_init(const io_chain_t *c, io_chain_iter_t *it);

/**
 * Get next span. Returns true if a span was written to out.
 */
bool io_chain_iter_next(io_chain_iter_t *it, io_span_t *out);

#ifdef __cplusplus
}
#endif

