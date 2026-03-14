#include "io_buf_pool.h"
#include "platform/os/os.h"
#include <string.h>
#include <stddef.h>

typedef struct io_pool_node {
    struct io_pool_node *next;
} io_pool_node_t;

typedef struct io_pool_slot {
    io_pool_node_t node;
    io_buf_t buf;
    /* payload follows */
} io_pool_slot_t;

struct io_buf_pool {
    size_t chunk_size;
    size_t chunk_count;

    io_allocator_t a;

    uint8_t *arena;          /* backing memory for nodes + chunk payloads */
    io_pool_node_t *free_list; /* points to slot->node */

    mimi_mutex_t *mu;
    volatile int available;
};

static void pool_release(io_buf_t *buf, void *owner)
{
    io_buf_pool_t *p = (io_buf_pool_t *)owner;
    if (!p || !buf) return;

    /* Reset public fields (keep base/capacity stable) */
    buf->len = 0;
    buf->refcount = 1;
    buf->release = pool_release;
    buf->owner = p;

    /* base points to payload; node is right before io_buf_t inside the slot */
    io_pool_slot_t *slot = (io_pool_slot_t *)((uint8_t *)buf - offsetof(io_pool_slot_t, buf));
    io_pool_node_t *node = &slot->node;

    mimi_mutex_lock(p->mu);
    node->next = p->free_list;
    p->free_list = node;
    __sync_fetch_and_add(&p->available, 1);
    mimi_mutex_unlock(p->mu);
}

static size_t node_stride(size_t chunk_size)
{
    /* slot header + chunk payload; align to pointer size */
    size_t n = sizeof(io_pool_slot_t) + chunk_size;
    size_t a = sizeof(void *);
    return (n + (a - 1)) & ~(a - 1);
}

io_buf_pool_t *io_buf_pool_create(size_t chunk_size, size_t chunk_count, const io_allocator_t *a)
{
    if (chunk_size == 0 || chunk_count == 0) return NULL;

    io_buf_pool_t *p = NULL;

    io_allocator_t alloc;
    if (a && a->alloc && a->free) {
        alloc = *a;
    } else {
        io_get_allocator(&alloc);
    }

    p = (io_buf_pool_t *)alloc.alloc(sizeof(*p), alloc.ctx);
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));

    p->chunk_size = chunk_size;
    p->chunk_count = chunk_count;
    p->a = alloc;
    p->available = (int)chunk_count;

    if (mimi_mutex_create(&p->mu) != MIMI_OK) {
        alloc.free(p, alloc.ctx);
        return NULL;
    }

    size_t stride = node_stride(chunk_size);
    size_t arena_size = stride * chunk_count;
    p->arena = (uint8_t *)alloc.alloc(arena_size, alloc.ctx);
    if (!p->arena) {
        mimi_mutex_destroy(p->mu);
        alloc.free(p, alloc.ctx);
        return NULL;
    }

    /* Build free list */
    p->free_list = NULL;
    for (size_t i = 0; i < chunk_count; i++) {
        io_pool_slot_t *slot = (io_pool_slot_t *)(p->arena + i * stride);
        slot->node.next = p->free_list;
        p->free_list = &slot->node;

        memset(&slot->buf, 0, sizeof(slot->buf));
        slot->buf.base = ((uint8_t *)slot) + sizeof(io_pool_slot_t);
        slot->buf.len = 0;
        slot->buf.capacity = chunk_size;
        slot->buf.refcount = 1;
        slot->buf.release = pool_release;
        slot->buf.owner = p;
    }

    return p;
}

void io_buf_pool_destroy(io_buf_pool_t *p)
{
    if (!p) return;

    if (p->mu) mimi_mutex_destroy(p->mu);
    if (p->arena) p->a.free(p->arena, p->a.ctx);
    p->a.free(p, p->a.ctx);
}

io_buf_t *io_buf_pool_alloc(io_buf_pool_t *p)
{
    if (!p) return NULL;

    mimi_mutex_lock(p->mu);
    io_pool_node_t *node = p->free_list;
    if (!node) {
        mimi_mutex_unlock(p->mu);
        return NULL;
    }
    p->free_list = node->next;
    __sync_fetch_and_add(&p->available, -1);
    mimi_mutex_unlock(p->mu);

    io_pool_slot_t *slot = (io_pool_slot_t *)((uint8_t *)node - offsetof(io_pool_slot_t, node));
    io_buf_t *b = &slot->buf;

    /* Reset and hand out */
    b->len = 0;
    b->capacity = p->chunk_size;
    b->refcount = 1;
    b->release = pool_release;
    b->owner = p;

    return b;
}

size_t io_buf_pool_chunk_size(const io_buf_pool_t *p)
{
    return p ? p->chunk_size : 0;
}

size_t io_buf_pool_capacity(const io_buf_pool_t *p)
{
    return p ? p->chunk_count : 0;
}

size_t io_buf_pool_available(const io_buf_pool_t *p)
{
    return p ? (size_t)p->available : 0;
}

