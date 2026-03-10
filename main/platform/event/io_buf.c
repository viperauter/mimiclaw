#include "io_buf.h"
#include <stdlib.h>
#include <string.h>

io_buf_t *io_buf_alloc(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }
    
    io_buf_t *buf = (io_buf_t *)malloc(sizeof(io_buf_t));
    if (!buf) {
        return NULL;
    }
    
    buf->base = (uint8_t *)malloc(capacity);
    if (!buf->base) {
        free(buf);
        return NULL;
    }
    
    buf->len = 0;
    buf->capacity = capacity;
    buf->refcount = 1;
    
    return buf;
}

io_buf_t *io_buf_from_data(void *data, size_t len)
{
    if (!data || len == 0) {
        return NULL;
    }
    
    io_buf_t *buf = (io_buf_t *)malloc(sizeof(io_buf_t));
    if (!buf) {
        return NULL;
    }
    
    buf->base = (uint8_t *)data;
    buf->len = len;
    buf->capacity = len;
    buf->refcount = 1;
    
    return buf;
}

io_buf_t *io_buf_from_const(const void *data, size_t len)
{
    if (!data || len == 0) {
        return NULL;
    }
    
    io_buf_t *buf = (io_buf_t *)malloc(sizeof(io_buf_t));
    if (!buf) {
        return NULL;
    }
    
    buf->base = (uint8_t *)malloc(len);
    if (!buf->base) {
        free(buf);
        return NULL;
    }
    
    memcpy(buf->base, data, len);
    buf->len = len;
    buf->capacity = len;
    buf->refcount = 1;
    
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
        if (buf->base) {
            free(buf->base);
        }
        free(buf);
    }
}

int io_buf_get_refcount(io_buf_t *buf)
{
    if (!buf) {
        return 0;
    }
    return buf->refcount;
}
