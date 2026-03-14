#include "io_chain.h"
#include "mimi_err.h"
#include <string.h>
#include <stdlib.h>

#define IO_CHAIN_DEFAULT_CHUNK 1024

typedef struct io_chain_node {
    struct io_chain_node *next;
    io_buf_t *buf;
} io_chain_node_t;

struct io_chain {
    volatile int refcount;
    io_buf_pool_t *pool;
    io_chain_node_t *head;
    io_chain_node_t *tail;
    size_t total_len;
    size_t chunks;
};

static io_chain_node_t *node_new(io_buf_t *buf)
{
    if (!buf) return NULL;
    io_chain_node_t *n = (io_chain_node_t *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->buf = buf;
    n->next = NULL;
    return n;
}

io_chain_t *io_chain_create(io_buf_pool_t *pool)
{
    io_chain_t *c = (io_chain_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->refcount = 1;
    c->pool = pool;
    return c;
}

io_chain_t *io_chain_ref(io_chain_t *c)
{
    if (!c) return NULL;
    __sync_fetch_and_add(&c->refcount, 1);
    return c;
}

void io_chain_unref(io_chain_t *c)
{
    if (!c) return;
    if (__sync_sub_and_fetch(&c->refcount, 1) != 0) return;

    io_chain_node_t *n = c->head;
    while (n) {
        io_chain_node_t *next = n->next;
        if (n->buf) io_buf_unref(n->buf);
        free(n);
        n = next;
    }
    free(c);
}

size_t io_chain_len(const io_chain_t *c)
{
    return c ? c->total_len : 0;
}

size_t io_chain_chunk_count(const io_chain_t *c)
{
    return c ? c->chunks : 0;
}

mimi_err_t io_chain_append_buf(io_chain_t *c, io_buf_t *buf)
{
    if (!c || !buf) return MIMI_ERR_INVALID_ARG;

    io_buf_ref(buf);
    io_chain_node_t *n = node_new(buf);
    if (!n) {
        io_buf_unref(buf);
        return MIMI_ERR_NO_MEM;
    }

    if (!c->head) {
        c->head = c->tail = n;
    } else {
        c->tail->next = n;
        c->tail = n;
    }

    c->total_len += buf->len;
    c->chunks += 1;
    return MIMI_OK;
}

static io_buf_t *alloc_chunk(io_chain_t *c, size_t min_capacity)
{
    if (c->pool) {
        io_buf_t *b = io_buf_pool_alloc(c->pool);
        if (!b) return NULL;
        /* If requested size is larger than pool chunk, caller will append in multiple chunks */
        (void)min_capacity;
        return b;
    }

    size_t cap = IO_CHAIN_DEFAULT_CHUNK;
    if (cap < min_capacity) cap = min_capacity;
    return io_buf_alloc(cap);
}

mimi_err_t io_chain_append_bytes(io_chain_t *c, const void *data, size_t len)
{
    if (!c) return MIMI_ERR_INVALID_ARG;
    if (!data && len) return MIMI_ERR_INVALID_ARG;
    if (len == 0) return MIMI_OK;

    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;

    while (left > 0) {
        io_buf_t *tail = c->tail ? c->tail->buf : NULL;
        size_t avail = 0;
        if (tail && tail->capacity >= tail->len) {
            avail = tail->capacity - tail->len;
        }

        if (!tail || avail == 0) {
            io_buf_t *b = alloc_chunk(c, left);
            if (!b) return MIMI_ERR_NO_MEM;

            io_chain_node_t *n = node_new(b);
            if (!n) {
                io_buf_unref(b);
                return MIMI_ERR_NO_MEM;
            }
            if (!c->head) c->head = c->tail = n;
            else { c->tail->next = n; c->tail = n; }
            c->chunks += 1;
            tail = b;
            avail = tail->capacity - tail->len;
        }

        size_t ncopy = left < avail ? left : avail;
        memcpy(tail->base + tail->len, p, ncopy);
        tail->len += ncopy;
        c->total_len += ncopy;
        p += ncopy;
        left -= ncopy;
    }

    return MIMI_OK;
}

size_t io_chain_to_spans(const io_chain_t *c, io_span_t *out, size_t out_cap)
{
    if (!c || !out || out_cap == 0) return 0;

    size_t nout = 0;
    for (io_chain_node_t *n = c->head; n && nout < out_cap; n = n->next) {
        if (!n->buf || n->buf->len == 0) continue;
        out[nout].base = n->buf->base;
        out[nout].len = n->buf->len;
        nout++;
    }
    return nout;
}

void io_chain_iter_init(const io_chain_t *c, io_chain_iter_t *it)
{
    if (!it) return;
    it->p = c ? (const void *)c->head : NULL;
}

bool io_chain_iter_next(io_chain_iter_t *it, io_span_t *out)
{
    if (!it || !out) return false;

    io_chain_node_t *n = (io_chain_node_t *)it->p;
    while (n) {
        it->p = (const void *)n->next;
        if (n->buf && n->buf->len > 0) {
            out->base = n->buf->base;
            out->len = n->buf->len;
            return true;
        }
        n = n->next;
    }
    it->p = NULL;
    return false;
}

