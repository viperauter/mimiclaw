#include "io_buf.h"
#include <stdlib.h>
#include <string.h>

static void *io_malloc(size_t size, void *ctx)
{
    (void)ctx;
    return malloc(size);
}

static void io_free(void *ptr, void *ctx)
{
    (void)ctx;
    free(ptr);
}

static io_allocator_t s_allocator = {
    .alloc = io_malloc,
    .free = io_free,
    .ctx = NULL,
};

void io_set_allocator(const io_allocator_t *a)
{
    if (!a || !a->alloc || !a->free) {
        s_allocator.alloc = io_malloc;
        s_allocator.free = io_free;
        s_allocator.ctx = NULL;
        return;
    }
    s_allocator = *a;
}

void io_get_allocator(io_allocator_t *out)
{
    if (!out) return;
    *out = s_allocator;
}

static void io_buf_default_release(io_buf_t *buf, void *owner)
{
    (void)owner;
    if (!buf) return;
    if (buf->base) {
        s_allocator.free(buf->base, s_allocator.ctx);
        buf->base = NULL;
    }
    s_allocator.free(buf, s_allocator.ctx);
}

io_buf_t *io_buf_alloc(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }
    
    io_buf_t *buf = (io_buf_t *)s_allocator.alloc(sizeof(io_buf_t), s_allocator.ctx);
    if (!buf) {
        return NULL;
    }
    
    buf->base = (uint8_t *)s_allocator.alloc(capacity, s_allocator.ctx);
    if (!buf->base) {
        s_allocator.free(buf, s_allocator.ctx);
        return NULL;
    }
    
    buf->len = 0;
    buf->capacity = capacity;
    buf->refcount = 1;
    buf->release = io_buf_default_release;
    buf->owner = NULL;
    
    return buf;
}

io_buf_t *io_buf_from_data(void *data, size_t len)
{
    if (!data || len == 0) {
        return NULL;
    }
    
    io_buf_t *buf = (io_buf_t *)s_allocator.alloc(sizeof(io_buf_t), s_allocator.ctx);
    if (!buf) {
        return NULL;
    }
    
    buf->base = (uint8_t *)data;
    buf->len = len;
    buf->capacity = len;
    buf->refcount = 1;
    buf->release = io_buf_default_release;
    buf->owner = NULL;
    
    return buf;
}

io_buf_t *io_buf_from_const(const void *data, size_t len)
{
    if (!data || len == 0) {
        return NULL;
    }
    
    io_buf_t *buf = (io_buf_t *)s_allocator.alloc(sizeof(io_buf_t), s_allocator.ctx);
    if (!buf) {
        return NULL;
    }
    
    buf->base = (uint8_t *)s_allocator.alloc(len, s_allocator.ctx);
    if (!buf->base) {
        s_allocator.free(buf, s_allocator.ctx);
        return NULL;
    }
    
    memcpy(buf->base, data, len);
    buf->len = len;
    buf->capacity = len;
    buf->refcount = 1;
    buf->release = io_buf_default_release;
    buf->owner = NULL;
    
    return buf;
}

io_buf_t *io_buf_ref(io_buf_t *buf)
{
    if (!buf) {
        return NULL;
    }
    
    __sync_fetch_and_add(&buf->refcount, 1);
    return buf;
}

void io_buf_unref(io_buf_t *buf)
{
    if (!buf) {
        return;
    }
    
    if (__sync_sub_and_fetch(&buf->refcount, 1) == 0) {
        if (buf->release) {
            buf->release(buf, buf->owner);
        } else {
            io_buf_default_release(buf, NULL);
        }
    }
}

int io_buf_get_refcount(io_buf_t *buf)
{
    if (!buf) {
        return 0;
    }
    return buf->refcount;
}
